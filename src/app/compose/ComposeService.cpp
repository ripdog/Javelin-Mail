#include "app/ComposeService.h"
#include "app/AccountConnectionProvider.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/ComposePreferences.h"
#include "app/DeferredSendService.h"
#include "app/MailApplicationPorts.h"
#include "app/WorkScheduler.h"
#include "app/undo/UndoManager.h"

#include "jmap/submission/ComposeService.h"
#include "jmap/submission/DraftSnapshotSerialization.h"

#include <QUuid>

#include <algorithm>
#include <chrono>

namespace javelin::app
{

    namespace
    {
        class ForegroundWorkScope final
        {
          public:
            explicit ForegroundWorkScope(WorkScheduler& scheduler) : m_scheduler(scheduler)
            {
                m_scheduler.beginForegroundWork();
            }
            ~ForegroundWorkScope()
            {
                m_scheduler.endForegroundWork();
            }

          private:
            WorkScheduler& m_scheduler;
        };

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(AccountConnectionSettings settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = std::move(settings.sessionUrl),
                .loginEmail = std::move(settings.loginEmail),
                .apiKey = std::move(settings.apiKey),
            };
        }
    } // namespace

    ComposeService::ComposeService(javelin::jmap::submission::ComposeService& service,
                                   ApplicationErrorCoordinator& errorCoordinator,
                                   WorkScheduler& workScheduler,
                                   AccountConnectionProvider& connectionProvider,
                                   MailCacheChangePublisher& cacheChangePublisher,
                                   javelin::app::undo::UndoManager& undoManager,
                                   DeferredSendService& deferredSendService)
        : m_service(service), m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler),
          m_connectionProvider(connectionProvider), m_cacheChangePublisher(cacheChangePublisher),
          m_undoManager(undoManager), m_deferredSendService(deferredSendService)
    {
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
    ComposeService::open(AccountConnectionSettings settings,
                         javelin::jmap::submission::OpenComposeRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto accountId = request.accountId;
        auto result =
            co_await m_service.open(toLiveConnectionSettings(settings), std::move(request));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Open composer"),
                                             *error);
        else
        {
            m_errorCoordinator.reportSuccess(settings.connectionId);
            const auto& snapshot = std::get<javelin::jmap::submission::DraftSnapshot>(result);
            if (snapshot.draftEmailId.has_value())
                m_lastSavedSnapshots.insert_or_assign(snapshot.composeSessionId, snapshot);
        }
        co_return result;
    }

    QCoro::Task<
        std::variant<std::vector<javelin::jmap::domain::Identity>, javelin::jmap::OperationError>>
    ComposeService::loadSenderIdentities(AccountConnectionSettings settings, std::string accountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto reportedAccountId = accountId;
        auto result = co_await m_service.loadSenderIdentities(toLiveConnectionSettings(settings),
                                                              std::move(accountId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, reportedAccountId,
                                             QStringLiteral("Load sender identities"), *error);
        else
            m_errorCoordinator.reportSuccess(settings.connectionId);
        co_return result;
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::OperationError>>
    ComposeService::saveDraft(AccountConnectionSettings settings,
                              javelin::jmap::submission::DraftSnapshot snapshot,
                              const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto accountId = snapshot.accountId;
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::jmap::submission::DraftSnapshot> before;
        const auto previous = m_lastSavedSnapshots.find(snapshot.composeSessionId);
        if (previous != m_lastSavedSnapshots.end())
            before = previous->second;
        else if (snapshot.draftEmailId.has_value())
        {
            auto authoritative = co_await m_service.loadAuthoritativeDraft(
                toLiveConnectionSettings(settings), snapshot.accountId, *snapshot.draftEmailId,
                snapshot.composeSessionId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&authoritative))
                co_return *error;
            before = std::get<javelin::jmap::submission::DraftSnapshot>(std::move(authoritative));
        }

        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            javelin::app::undo::DraftHistory history{
                .connectionId = settings.connectionId,
                .accountId = snapshot.accountId,
                .composeSessionId = snapshot.composeSessionId,
                .currentDraftEmailId = snapshot.draftEmailId,
                .beforeSnapshotJson =
                    before.has_value()
                        ? std::optional{javelin::jmap::submission::serializeDraftSnapshot(*before)
                                            .toStdString()}
                        : std::nullopt,
                .afterSnapshotJson =
                    javelin::jmap::submission::serializeDraftSnapshot(snapshot).toStdString(),
            };
            const auto subject = snapshot.subject.value_or(std::string{});
            auto preparedResult = m_undoManager.prepareNormal(
                subject.empty()
                    ? QStringLiteral("Save Draft")
                    : QStringLiteral("Save Draft “%1”").arg(QString::fromStdString(subject)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }

        auto savedSnapshot = snapshot;
        auto result =
            co_await m_service.saveDraft(toLiveConnectionSettings(settings), std::move(snapshot),
                                         operationGroupId.toStdString());
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Save draft"),
                                             *error);
            if (prepared.has_value())
            {
                if (javelin::jmap::isTransientError(*error) &&
                    !javelin::jmap::isAuthenticationError(*error))
                {
                    auto committed = m_undoManager.commitNormalBlockedUnknown(std::move(*prepared),
                                                                              error->message);
                    if (const auto* databaseError =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                        co_return javelin::jmap::operationError(*databaseError);
                }
                else if (const auto databaseError = m_undoManager.discardNormal(prepared->entryId))
                    co_return javelin::jmap::operationError(*databaseError);
            }
        }
        else
        {
            m_errorCoordinator.reportSuccess(settings.connectionId);
            const auto& summary = std::get<javelin::jmap::submission::DraftSaveSummary>(result);
            savedSnapshot = summary.savedSnapshot;
            QStringList affectedMailboxIds;
            affectedMailboxIds.reserve(static_cast<qsizetype>(summary.affectedMailboxIds.size()));
            for (const auto& mailboxId : summary.affectedMailboxIds)
                affectedMailboxIds.push_back(QString::fromStdString(mailboxId));
            m_cacheChangePublisher.publishCacheChange({
                .accountId = QString::fromStdString(summary.accountId),
                .mailboxIds = std::move(affectedMailboxIds),
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = false,
                .hasNewMail = false,
                .optimisticProjection = true,
            });
            m_lastSavedSnapshots.insert_or_assign(savedSnapshot.composeSessionId, savedSnapshot);
            if (prepared.has_value())
            {
                auto& history = std::get<javelin::app::undo::DraftHistory>(prepared->payload);
                history.currentDraftEmailId = summary.draftEmailId;
                history.afterSnapshotJson =
                    javelin::jmap::submission::serializeDraftSnapshot(savedSnapshot).toStdString();
                auto committed = m_undoManager.commitNormal(std::move(*prepared));
                if (const auto* databaseError =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                    co_return javelin::jmap::operationError(*databaseError);
            }
        }
        co_return result;
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSnapshot, javelin::jmap::OperationError>>
    ComposeService::loadAuthoritativeDraft(std::string accountId, std::string draftEmailId,
                                           std::string composeSessionId)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(accountId);
        if (!settings.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return co_await m_service.loadAuthoritativeDraft(
            toLiveConnectionSettings(*settings), std::move(accountId), std::move(draftEmailId),
            std::move(composeSessionId));
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftSaveSummary, javelin::jmap::OperationError>>
    ComposeService::saveDraftFromHistory(javelin::jmap::submission::DraftSnapshot snapshot,
                                         const javelin::app::undo::CommandOrigin origin)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(snapshot.accountId);
        if (!settings.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return co_await saveDraft(*settings, std::move(snapshot), origin);
    }

    QCoro::Task<
        std::variant<javelin::jmap::submission::DraftDeleteSummary, javelin::jmap::OperationError>>
    ComposeService::deleteDraftFromHistory(std::string accountId, std::string draftEmailId,
                                           const javelin::app::undo::CommandOrigin)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(accountId);
        if (!settings.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        const auto operationGroupId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto removedDraftEmailId = draftEmailId;
        auto result = co_await m_service.deleteDraft(toLiveConnectionSettings(*settings), accountId,
                                                     std::move(draftEmailId), operationGroupId);
        if (std::holds_alternative<javelin::jmap::submission::DraftDeleteSummary>(result))
        {
            std::erase_if(m_lastSavedSnapshots,
                          [&](const auto& value)
                          {
                              return value.second.accountId == accountId &&
                                     value.second.draftEmailId == removedDraftEmailId;
                          });
        }
        co_return result;
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    ComposeService::send(AccountConnectionSettings settings,
                         javelin::jmap::submission::DraftSnapshot snapshot)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto accountId = snapshot.accountId;
        const auto composeSessionId = snapshot.composeSessionId;
        auto prepared =
            co_await m_service.prepareSend(toLiveConnectionSettings(settings), std::move(snapshot));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
        {
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Prepare message"),
                                             *error);
            co_return *error;
        }
        auto result = co_await m_deferredSendService.schedule(
            settings.connectionId,
            std::get<javelin::jmap::submission::PreparedSend>(std::move(prepared)),
            std::chrono::seconds{ComposePreferences::undoSendDelaySeconds()});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Send message"),
                                             *error);
        else
        {
            m_errorCoordinator.reportSuccess(settings.connectionId);
            m_lastSavedSnapshots.erase(composeSessionId);
        }
        co_return result;
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    ComposeService::scheduleSend(AccountConnectionSettings settings,
                                 javelin::jmap::submission::ScheduledSendRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto accountId = request.snapshot.accountId;
        const auto composeSessionId = request.snapshot.composeSessionId;
        if (const auto error = m_service.validateScheduledSend(accountId, request.sendAt))
        {
            m_errorCoordinator.reportFailure(settings, accountId,
                                             QStringLiteral("Schedule message"), *error);
            co_return *error;
        }

        auto prepared = co_await m_service.prepareSend(toLiveConnectionSettings(settings),
                                                       std::move(request.snapshot));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&prepared))
        {
            m_errorCoordinator.reportFailure(settings, accountId, QStringLiteral("Prepare message"),
                                             *error);
            co_return *error;
        }
        auto result = co_await m_service.submitPreparedSendAt(
            toLiveConnectionSettings(settings),
            std::get<javelin::jmap::submission::PreparedSend>(std::move(prepared)), request.sendAt);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            m_errorCoordinator.reportFailure(settings, accountId,
                                             QStringLiteral("Schedule message"), *error);
        else
        {
            m_errorCoordinator.reportSuccess(settings.connectionId);
            m_lastSavedSnapshots.erase(composeSessionId);
        }
        co_return result;
    }

    std::variant<bool, javelin::jmap::OperationError>
    ComposeService::cancelDeferredSend(const QString& sendId)
    {
        return m_deferredSendService.cancelTargeted(sendId);
    }

    std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>,
                 javelin::jmap::OperationError>
    ComposeService::loadWorkingCopy(const std::string_view composeSessionId) const
    {
        return m_service.loadWorkingCopy(composeSessionId);
    }

    std::optional<javelin::jmap::OperationError>
    ComposeService::storeWorkingCopy(const javelin::jmap::submission::DraftSnapshot& snapshot)
    {
        const auto staleSend = std::ranges::find_if(
            m_undoManager.entries(),
            [&](const javelin::app::undo::HistoryEntry& entry)
            {
                const auto* send =
                    std::get_if<javelin::app::undo::DeferredSendHistory>(&entry.payload);
                return entry.stack == javelin::app::undo::HistoryStack::Redo && send != nullptr &&
                       send->composeSessionId == snapshot.composeSessionId;
            });
        if (staleSend != m_undoManager.entries().end())
        {
            if (const auto error = m_undoManager.forgetAndClearRedo(staleSend->entryId))
                return javelin::jmap::operationError(*error);
        }
        return m_service.storeWorkingCopy(snapshot);
    }

    std::optional<javelin::jmap::OperationError>
    ComposeService::discard(const std::string_view composeSessionId)
    {
        auto result = m_service.discard(composeSessionId);
        if (!result.has_value())
            m_lastSavedSnapshots.erase(std::string{composeSessionId});
        return result;
    }

} // namespace javelin::app
