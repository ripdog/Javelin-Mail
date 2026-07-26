#include "app/undo/ContactHistoryExecutor.h"

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace javelin::app::undo
{
    namespace
    {
        [[nodiscard]] HistoryExecutionOutcome outcomeFor(const javelin::jmap::OperationError& error)
        {
            using enum javelin::jmap::OperationErrorCode;
            if (error.code == Conflict || error.code == PreconditionFailed ||
                error.code == NotFound)
                return HistoryExecutionOutcome::Conflict;
            if (javelin::jmap::isTransientError(error))
                return HistoryExecutionOutcome::Unknown;
            return HistoryExecutionOutcome::DefinitiveFailure;
        }

        [[nodiscard]] HistoryExecutionResult failure(const javelin::jmap::OperationError& error)
        {
            return {
                .outcome = outcomeFor(error),
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = error.message,
                .objectFailures = {},
                .mayRemoveFromHistory = !javelin::jmap::isTransientError(error),
            };
        }

        [[nodiscard]] HistoryExecutionResult conflict(QString message)
        {
            return {
                .outcome = HistoryExecutionOutcome::Conflict,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(message),
                .objectFailures = {},
                .mayRemoveFromHistory = true,
            };
        }

        [[nodiscard]] HistoryExecutionResult success(ContactCardHistory history)
        {
            const auto accountId = QString::fromStdString(history.accountId);
            return {
                .outcome = HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {accountId},
                        .objectTypes = {QStringLiteral("ContactCard")},
                        .views = {QStringLiteral("contacts")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }

        [[nodiscard]] std::optional<std::string>
        normalized(const std::optional<std::string>& document, const bool creating = false)
        {
            if (!document.has_value())
                return std::nullopt;
            auto prepared = javelin::jmap::contacts::prepareContactDocument(*document, creating);
            if (const auto* value = std::get_if<std::string>(&prepared))
                return *value;
            return std::nullopt;
        }

        [[nodiscard]] const javelin::jmap::contacts::ContactSummary*
        findCurrent(const AuthoritativeContacts& authoritative, const ContactCardItemHistory& item)
        {
            if (item.currentCardId.has_value())
            {
                const auto found = std::ranges::find(authoritative.contacts, *item.currentCardId,
                                                     &javelin::jmap::contacts::ContactSummary::id);
                if (found != authoritative.contacts.end())
                    return &*found;
            }
            const auto found = std::ranges::find(authoritative.contacts, item.uid,
                                                 &javelin::jmap::contacts::ContactSummary::uid);
            return found == authoritative.contacts.end() ? nullptr : &*found;
        }
    } // namespace

    ContactHistoryExecutor::ContactHistoryExecutor(ContactHistoryPort& contacts)
        : m_contacts(contacts)
    {
    }

    QCoro::Task<HistoryExecutionResult>
    ContactHistoryExecutor::execute(HistoryEntry entry, const HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<ContactCardHistory>(&entry.payload);
        if (history == nullptr || direction == HistoryExecutionDirection::Recover)
            co_return conflict(
                QStringLiteral("The contact operation requires authoritative reconciliation."));
        if (history->items.empty())
            co_return conflict(QStringLiteral("The contact history payload is empty."));

        const bool undo = direction == HistoryExecutionDirection::Undo;
        auto loaded =
            co_await m_contacts.getAuthoritativeContacts(history->connectionId, history->accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
            co_return failure(*error);
        const auto& authoritative = std::get<AuthoritativeContacts>(loaded);

        javelin::jmap::api::ContactCardSetRequest request{
            .accountId = history->accountId,
            .ifInState =
                authoritative.state.empty() ? std::nullopt : std::optional{authoritative.state},
            .create = {},
            .update = {},
            .destroy = {},
        };
        std::unordered_map<std::string, std::size_t> creationItems;
        for (std::size_t index = 0; index < history->items.size(); ++index)
        {
            auto& item = history->items[index];
            const auto& expectedJson = undo ? item.afterDocumentJson : item.beforeDocumentJson;
            const auto& desiredJson = undo ? item.beforeDocumentJson : item.afterDocumentJson;
            const auto expected = normalized(expectedJson);
            const auto desired = normalized(desiredJson);
            if (expectedJson.has_value() != expected.has_value() ||
                desiredJson.has_value() != desired.has_value())
                co_return conflict(QStringLiteral("The contact history payload is invalid."));

            const auto* current = findCurrent(authoritative, item);
            if (expected.has_value())
            {
                if (current == nullptr)
                    co_return conflict(
                        QStringLiteral("A contact in this operation is no longer available."));
                const auto currentDocument =
                    normalized(std::optional<std::string>{current->document});
                if (!currentDocument.has_value() || *currentDocument != *expected)
                    co_return conflict(
                        QStringLiteral("A contact in this operation changed on another client."));
                item.currentCardId = current->id;
            }
            else if (current != nullptr)
                co_return conflict(
                    QStringLiteral("A contact exists where this history expected none."));

            if (!desired.has_value())
            {
                if (current == nullptr)
                    co_return conflict(
                        QStringLiteral("The contact history item has no before or after state."));
                request.destroy.push_back(current->id);
                continue;
            }
            if (current != nullptr)
            {
                request.update.emplace(current->id,
                                       javelin::jmap::api::ContactDocument{.json = *desired});
                continue;
            }

            const auto creationId = "history-" + std::to_string(index);
            const auto creationDocument = normalized(desiredJson, true);
            if (!creationDocument.has_value())
                co_return conflict(
                    QStringLiteral("A recreated contact is missing required properties."));
            request.create.emplace(creationId,
                                   javelin::jmap::api::ContactDocument{.json = *creationDocument});
            creationItems.emplace(creationId, index);
        }

        auto mutated = co_await m_contacts.applyContactCardsFromHistory(
            history->connectionId, std::move(request),
            undo ? CommandOrigin::Undo : CommandOrigin::Redo);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&mutated))
            co_return failure(*error);
        const auto& summary = std::get<javelin::jmap::contacts::ContactMutationSummary>(mutated);
        for (auto& item : history->items)
            if ((undo ? item.beforeDocumentJson : item.afterDocumentJson) == std::nullopt)
                item.currentCardId = std::nullopt;
        for (const auto& mapping : summary.createdIds)
            if (const auto found = creationItems.find(mapping.creationId);
                found != creationItems.end())
                history->items[found->second].currentCardId = mapping.serverId;
        if (summary.createdIds.size() != creationItems.size())
            co_return conflict(
                QStringLiteral("The recreated contacts have incomplete server identities."));
        co_return success(*history);
    }
} // namespace javelin::app::undo
