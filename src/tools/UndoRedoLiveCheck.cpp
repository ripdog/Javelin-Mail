#include "app/ComposeService.h"
#include "app/ContactApplicationPorts.h"
#include "app/DaemonServices.h"
#include "app/DeferredSendService.h"
#include "app/MailApplicationService.h"
#include "app/undo/AddressBookHistoryExecutor.h"
#include "app/undo/AddressBookHistoryPort.h"
#include "app/undo/CalendarHistoryExecutor.h"
#include "app/undo/CalendarPreferenceExecutor.h"
#include "app/undo/ContactHistoryExecutor.h"
#include "app/undo/ContactHistoryPort.h"
#include "app/undo/DraftHistoryExecutor.h"
#include "app/undo/MailHistoryExecutor.h"
#include "app/undo/SieveHistoryExecutor.h"
#include "app/undo/UndoManager.h"
#include "tools/UndoRedoAutonomousSuite.h"

#include <QCoroSignal>
#include <QCoroTask>

#include <QApplication>
#include <QCommandLineParser>
#include <QSettings>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using javelin::app::AccountConnectionSettings;
    using javelin::app::AccountSyncConfiguration;
    using javelin::app::undo::HistoryCommandExecutor;
    using javelin::app::undo::HistoryEntry;
    using javelin::app::undo::HistoryExecutionDirection;
    using javelin::app::undo::HistoryExecutionOutcome;
    using javelin::app::undo::HistoryStack;

    constexpr std::array supportedKinds{
        std::string_view{"mail_patch"},     std::string_view{"draft"},
        std::string_view{"sieve"},          std::string_view{"deferred_send"},
        std::string_view{"calendar_event"}, std::string_view{"calendar_preference"},
        std::string_view{"contact_card"},   std::string_view{"address_book"},
    };

    struct ConfiguredConnection
    {
        AccountConnectionSettings settings;
        std::vector<std::string> accountIds;
    };

    struct CheckResult
    {
        QString entryId;
        QString commandKind;
        bool passed = false;
        QString detail;
    };

    [[nodiscard]] std::vector<ConfiguredConnection> configuredConnections()
    {
        QSettings settings{QStringLiteral("Javelin Mail"), QStringLiteral("javelinmail")};
        settings.beginGroup(QStringLiteral("accounts"));
        const int count = settings.beginReadArray(QStringLiteral("size"));
        std::vector<ConfiguredConnection> result;
        result.reserve(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index)
        {
            settings.setArrayIndex(index);
            const auto cachedIds =
                settings.value(QStringLiteral("cachedAccountIds")).toStringList();
            std::vector<std::string> accountIds;
            accountIds.reserve(static_cast<std::size_t>(cachedIds.size()));
            for (const auto& id : cachedIds)
                accountIds.push_back(id.toStdString());
            const auto connectionId = settings.value(QStringLiteral("id")).toString().toStdString();
            const auto sessionUrl =
                settings.value(QStringLiteral("sessionUrl")).toString().toStdString();
            const auto loginEmail =
                settings.value(QStringLiteral("loginEmail")).toString().toStdString();
            const auto apiKey = settings.value(QStringLiteral("apiKey")).toString().toStdString();
            if (connectionId.empty() || sessionUrl.empty() || loginEmail.empty() ||
                apiKey.empty() || accountIds.empty())
                continue;
            result.push_back({
                .settings =
                    {
                        .connectionId = connectionId,
                        .revision = settings.value(QStringLiteral("revision"), 1).toULongLong(),
                        .sessionUrl = sessionUrl,
                        .loginEmail = loginEmail,
                        .apiKey = apiKey,
                    },
                .accountIds = std::move(accountIds),
            });
        }
        settings.endArray();
        settings.endGroup();
        return result;
    }

    [[nodiscard]] std::vector<AccountSyncConfiguration>
    syncConfigurations(const std::vector<ConfiguredConnection>& connections)
    {
        std::vector<AccountSyncConfiguration> result;
        for (const auto& connection : connections)
            for (const auto& accountId : connection.accountIds)
                result.push_back({
                    .settings = connection.settings,
                    .accountId = accountId,
                    .mailboxIds = {},
                    .fullSyncMailboxIds = {},
                    .notificationMailboxIds = {},
                    .notificationMailboxSelectionConfigured = false,
                });
        return result;
    }

    [[nodiscard]] QString outcomeName(const HistoryExecutionOutcome outcome)
    {
        switch (outcome)
        {
        case HistoryExecutionOutcome::Success:
            return QStringLiteral("success");
        case HistoryExecutionOutcome::Conflict:
            return QStringLiteral("conflict");
        case HistoryExecutionOutcome::DefinitiveFailure:
            return QStringLiteral("definitive failure");
        case HistoryExecutionOutcome::Unknown:
            return QStringLiteral("unknown");
        case HistoryExecutionOutcome::PartialFailure:
            return QStringLiteral("partial failure");
        case HistoryExecutionOutcome::Impossible:
            return QStringLiteral("impossible");
        case HistoryExecutionOutcome::Expired:
            return QStringLiteral("expired");
        }
        return QStringLiteral("unrecognized");
    }

    [[nodiscard]] HistoryExecutionDirection firstDirection(const HistoryEntry& entry)
    {
        return entry.stack == HistoryStack::Undo ? HistoryExecutionDirection::Undo
                                                 : HistoryExecutionDirection::Redo;
    }

    [[nodiscard]] HistoryExecutionDirection opposite(const HistoryExecutionDirection direction)
    {
        return direction == HistoryExecutionDirection::Undo ? HistoryExecutionDirection::Redo
                                                            : HistoryExecutionDirection::Undo;
    }

    [[nodiscard]] QCoro::Task<CheckResult> checkEntry(HistoryCommandExecutor& executor,
                                                      HistoryEntry entry, const int cycles)
    {
        const auto originalId = entry.entryId;
        const auto commandKind = entry.commandKind;
        const auto forward = firstDirection(entry);
        const auto backward = opposite(forward);
        for (int cycle = 0; cycle < cycles; ++cycle)
        {
            auto first = co_await executor.execute(entry, forward);
            if (first.outcome != HistoryExecutionOutcome::Success ||
                !first.updatedPayload.has_value())
            {
                co_return CheckResult{
                    .entryId = originalId,
                    .commandKind = commandKind,
                    .passed = false,
                    .detail = QStringLiteral("first leg: %1 — %2")
                                  .arg(outcomeName(first.outcome), first.summary),
                };
            }
            entry.payload = std::move(*first.updatedPayload);

            auto second = co_await executor.execute(entry, backward);
            if (second.outcome != HistoryExecutionOutcome::Success ||
                !second.updatedPayload.has_value())
            {
                auto cleanup = co_await executor.execute(entry, backward);
                const auto cleanupDetail =
                    cleanup.outcome == HistoryExecutionOutcome::Success
                        ? QStringLiteral("cleanup restored the starting state")
                        : QStringLiteral("CLEANUP FAILED: %1 — %2")
                              .arg(outcomeName(cleanup.outcome), cleanup.summary);
                co_return CheckResult{
                    .entryId = originalId,
                    .commandKind = commandKind,
                    .passed = false,
                    .detail = QStringLiteral("return leg: %1 — %2; %3")
                                  .arg(outcomeName(second.outcome), second.summary, cleanupDetail),
                };
            }
            entry.payload = std::move(*second.updatedPayload);
        }
        co_return CheckResult{
            .entryId = originalId,
            .commandKind = commandKind,
            .passed = true,
            .detail = QStringLiteral("%1 complete round-trip cycle(s)").arg(cycles),
        };
    }

    class ExecutorRegistry
    {
      public:
        explicit ExecutorRegistry(javelin::app::DaemonServices& services)
            : m_draft{services.composeService()}, m_mail{services.mailService()},
              m_sieve{services.mailService()}, m_calendar{services.mailService()},
              m_calendarPreference{services.mailService()},
              m_contactPort{dynamic_cast<javelin::app::undo::ContactHistoryPort*>(
                  &services.contactCommandPort())},
              m_addressBookPort{dynamic_cast<javelin::app::undo::AddressBookHistoryPort*>(
                  &services.contactCommandPort())},
              m_contact{required(m_contactPort, "contact history port")},
              m_addressBook{required(m_addressBookPort, "address-book history port"),
                            required(m_contactPort, "contact history port")},
              m_deferred{services.deferredSendService()}
        {
            m_executors.emplace(QStringLiteral("mail_patch"), &m_mail);
            m_executors.emplace(QStringLiteral("draft"), &m_draft);
            m_executors.emplace(QStringLiteral("sieve"), &m_sieve);
            m_executors.emplace(QStringLiteral("deferred_send"), &m_deferred);
            m_executors.emplace(QStringLiteral("calendar_event"), &m_calendar);
            m_executors.emplace(QStringLiteral("calendar_preference"), &m_calendarPreference);
            m_executors.emplace(QStringLiteral("contact_card"), &m_contact);
            m_executors.emplace(QStringLiteral("address_book"), &m_addressBook);
        }

        [[nodiscard]] HistoryCommandExecutor* find(const QString& commandKind) const
        {
            const auto found = m_executors.find(commandKind);
            return found == m_executors.end() ? nullptr : found->second;
        }

      private:
        template <typename Port> [[nodiscard]] static Port& required(Port* port, const char* name)
        {
            if (port == nullptr)
                throw std::runtime_error(std::string{"DaemonServices did not expose "} + name);
            return *port;
        }

        javelin::app::undo::DraftHistoryExecutor m_draft;
        javelin::app::undo::MailHistoryExecutor m_mail;
        javelin::app::undo::SieveHistoryExecutor m_sieve;
        javelin::app::undo::CalendarHistoryExecutor m_calendar;
        javelin::app::undo::CalendarPreferenceExecutor m_calendarPreference;
        javelin::app::undo::ContactHistoryPort* m_contactPort;
        javelin::app::undo::AddressBookHistoryPort* m_addressBookPort;
        javelin::app::undo::ContactHistoryExecutor m_contact;
        javelin::app::undo::AddressBookHistoryExecutor m_addressBook;
        javelin::app::DeferredSendService& m_deferred;
        std::unordered_map<QString, HistoryCommandExecutor*> m_executors;
    };

    [[nodiscard]] std::vector<HistoryEntry>
    selectedEntries(const std::vector<HistoryEntry>& entries, const QString& entryId,
                    const QString& commandKind, const bool all)
    {
        std::vector<HistoryEntry> selected;
        for (const auto& entry : entries)
        {
            if (!entryId.isEmpty() && entry.entryId != entryId)
                continue;
            if (!commandKind.isEmpty() && entry.commandKind != commandKind)
                continue;
            if (!all && entryId.isEmpty() && commandKind.isEmpty())
                continue;
            if (entry.status != javelin::app::undo::HistoryEntryStatus::Ready)
                continue;
            selected.push_back(entry);
        }
        std::ranges::sort(selected, std::greater{}, &HistoryEntry::stackOrder);
        if (all)
        {
            std::unordered_set<std::string> retainedKinds;
            std::erase_if(
                selected, [&retainedKinds](const HistoryEntry& entry)
                { return !retainedKinds.insert(entry.commandKind.toStdString()).second; });
        }
        return selected;
    }

    [[nodiscard]] std::vector<std::string_view>
    missingKinds(const std::vector<HistoryEntry>& entries)
    {
        std::unordered_set<std::string> present;
        for (const auto& entry : entries)
            present.insert(entry.commandKind.toStdString());
        std::vector<std::string_view> missing;
        for (const auto kind : supportedKinds)
            if (!present.contains(std::string{kind}))
                missing.push_back(kind);
        return missing;
    }

    void printInventory(const std::vector<HistoryEntry>& entries)
    {
        std::unordered_set<std::string> present;
        for (const auto& entry : entries)
        {
            if (entry.status != javelin::app::undo::HistoryEntryStatus::Ready)
                continue;
            present.insert(entry.commandKind.toStdString());
            std::cout << entry.entryId.toStdString() << '\t' << entry.commandKind.toStdString()
                      << '\t' << javelin::app::undo::toString(entry.stack).toStdString() << '\t'
                      << entry.label.toStdString() << '\n';
        }
        std::cout << "\nCoverage samples:\n";
        for (const auto kind : supportedKinds)
            std::cout << (present.contains(std::string{kind}) ? "  available  " : "  MISSING    ")
                      << kind << '\n';
    }
} // namespace

int main(int argc, char* argv[])
{
    QApplication application{argc, argv};
    application.setApplicationName(QStringLiteral("Javelin Mail"));
    application.setOrganizationName(QStringLiteral("Javelin Mail"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Round-trip persisted undo/redo entries against the configured live JMAP server."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("entry"), QStringLiteral("Check one history entry."),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("kind"),
                      QStringLiteral("Check ready entries of one command kind."),
                      QStringLiteral("command-kind")});
    parser.addOption(
        {QStringLiteral("all"),
         QStringLiteral("Check the newest ready sample of every supported command kind.")});
    parser.addOption(
        {QStringLiteral("autonomous"),
         QStringLiteral("Create isolated fixtures and exercise the autonomous live suite.")});
    parser.addOption({QStringLiteral("connection"),
                      QStringLiteral("Connection id for autonomous fixtures."),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("cycles"),
                      QStringLiteral("Round-trip cycles per entry (default 2)."),
                      QStringLiteral("count"), QStringLiteral("2")});
    parser.addOption(
        {QStringLiteral("execute-live-mutations"),
         QStringLiteral("Required acknowledgement that server objects will change temporarily.")});
    parser.process(application);

    try
    {
        bool validCycles = false;
        const int cycles = parser.value(QStringLiteral("cycles")).toInt(&validCycles);
        if (!validCycles || cycles < 1 || cycles > 10)
            throw std::runtime_error("--cycles must be between 1 and 10");

        const auto connections = configuredConnections();
        if (connections.empty())
            throw std::runtime_error("No complete configured Javelin connection is available");

        javelin::app::DaemonServices services;
        services.mailService().applySettings(syncConfigurations(connections));
        const bool autonomous = parser.isSet(QStringLiteral("autonomous"));
        if (autonomous)
        {
            if (!parser.isSet(QStringLiteral("execute-live-mutations")))
                throw std::runtime_error(
                    "Refusing to mutate the live server without --execute-live-mutations");
            const auto requestedConnection = parser.value(QStringLiteral("connection"));
            const auto selectedConnection = std::ranges::find_if(
                connections,
                [&requestedConnection](const ConfiguredConnection& connection)
                {
                    return requestedConnection.isEmpty() ||
                           connection.settings.connectionId == requestedConnection.toStdString();
                });
            if (selectedConnection == connections.end())
                throw std::runtime_error("No configured connection matches --connection");
            int exitCode = 1;
            auto task = [&]() -> QCoro::Task<void>
            {
                exitCode = co_await javelin::tools::runUndoRedoAutonomousSuite(
                    services, {
                                  .connectionId = selectedConnection->settings.connectionId,
                                  .accountIds = selectedConnection->accountIds,
                              });
            }();
            QCoro::connect(std::move(task), &application, &QCoreApplication::quit);
            static_cast<void>(application.exec());
            return exitCode;
        }
        const auto& entries = services.undoManager().entries();
        const auto entryId = parser.value(QStringLiteral("entry"));
        const auto commandKind = parser.value(QStringLiteral("kind"));
        const bool all = parser.isSet(QStringLiteral("all"));
        if (entryId.isEmpty() && commandKind.isEmpty() && !all)
        {
            printInventory(entries);
            return 0;
        }
        if (!parser.isSet(QStringLiteral("execute-live-mutations")))
            throw std::runtime_error(
                "Refusing to mutate the live server without --execute-live-mutations");

        auto selected = selectedEntries(entries, entryId, commandKind, all);
        if (selected.empty())
            throw std::runtime_error("No matching ready history entries were found");

        ExecutorRegistry executors{services};
        const auto missing = all ? missingKinds(selected) : std::vector<std::string_view>{};
        int exitCode = missing.empty() ? 0 : 1;
        auto task = [&]() -> QCoro::Task<void>
        {
            for (const auto kind : missing)
                std::cout << "MISSING\t-\t" << kind << "\tno ready history sample\n";
            for (auto& entry : selected)
            {
                auto* executor = executors.find(entry.commandKind);
                if (executor == nullptr)
                {
                    std::cout << "SKIP\t" << entry.entryId.toStdString() << '\t'
                              << entry.commandKind.toStdString() << "\tno registered executor\n";
                    exitCode = 1;
                    continue;
                }
                const auto result = co_await checkEntry(*executor, std::move(entry), cycles);
                std::cout << (result.passed ? "PASS" : "FAIL") << '\t'
                          << result.entryId.toStdString() << '\t'
                          << result.commandKind.toStdString() << '\t' << result.detail.toStdString()
                          << '\n';
                if (!result.passed)
                    exitCode = 1;
            }
        }();
        QCoro::connect(std::move(task), &application, &QCoreApplication::quit);
        static_cast<void>(application.exec());
        return exitCode;
    }
    catch (const std::exception& error)
    {
        std::cerr << "javelin-undo-live-check: " << error.what() << '\n';
        return 1;
    }
}
