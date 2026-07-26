#include "app/DeferredSendService.h"

#include "app/undo/UndoManager.h"

#include <QCoroCore>

#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        liveSettings(const AccountConnectionSettings& settings)
        {
            return {
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        [[nodiscard]] QString sendLabel(const std::optional<std::string>& subject)
        {
            return subject.has_value() && !subject->empty()
                       ? QStringLiteral("Send “%1”").arg(QString::fromStdString(*subject))
                       : QStringLiteral("Send Message");
        }

        [[nodiscard]] javelin::app::undo::HistoryExecutionResult
        historyFailure(QString message, const javelin::app::undo::HistoryExecutionOutcome outcome)
        {
            return {
                .outcome = outcome,
                .updatedPayload = std::nullopt,
                .refreshScope = {},
                .summary = std::move(message),
                .objectFailures = {},
                .mayRemoveFromHistory =
                    outcome == javelin::app::undo::HistoryExecutionOutcome::DefinitiveFailure,
            };
        }

        [[nodiscard]] javelin::app::undo::HistoryExecutionResult
        historySuccess(javelin::app::undo::DeferredSendHistory history)
        {
            const auto accountId = QString::fromStdString(history.accountId);
            return {
                .outcome = javelin::app::undo::HistoryExecutionOutcome::Success,
                .updatedPayload = std::move(history),
                .refreshScope =
                    {
                        .accountIds = {accountId},
                        .objectTypes = {QStringLiteral("Email")},
                        .views = {QStringLiteral("compose"), QStringLiteral("drafts")},
                    },
                .summary = {},
                .objectFailures = {},
                .mayRemoveFromHistory = false,
            };
        }
    } // namespace

    DeferredSendService::DeferredSendService(
        DeferredSendRepository& repository,
        javelin::jmap::submission::ComposeService& composeService,
        AccountConnectionProvider& connectionProvider, javelin::app::undo::UndoManager& undoManager,
        std::function<QDateTime()> clock, QObject* parent)
        : QObject(parent), m_repository(repository), m_composeService(composeService),
          m_connectionProvider(connectionProvider), m_undoManager(undoManager),
          m_clock(std::move(clock))
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, &DeferredSendService::dispatchDue);
    }

    void DeferredSendService::start()
    {
        static_cast<void>(m_repository.recoverDispatching());
        scheduleNext();
    }

    QCoro::Task<std::variant<javelin::jmap::submission::SendSummary, javelin::jmap::OperationError>>
    DeferredSendService::schedule(std::string connectionId,
                                  javelin::jmap::submission::PreparedSend prepared,
                                  const std::chrono::seconds delay)
    {
        const auto sendId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto dueAt = now().addSecs(delay.count());
        const auto& snapshot = prepared.draft.savedSnapshot;
        javelin::app::undo::DeferredSendHistory history{
            .sendId = sendId.toStdString(),
            .connectionId = connectionId,
            .accountId = prepared.draft.accountId,
            .composeSessionId = prepared.draft.composeSessionId,
            .draftEmailId = prepared.draft.draftEmailId,
            .subject = snapshot.subject,
            .delaySeconds = delay.count(),
        };
        auto preparedHistory = m_undoManager.prepareNormal(
            sendLabel(snapshot.subject), javelin::app::undo::HistoryDomain::DeferredSend, history,
            std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedHistory))
            co_return javelin::jmap::operationError(*error);
        auto reservation =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedHistory));
        if (!reservation.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = QStringLiteral("Unable to reserve Undo Send history."),
            };

        PendingSend send{
            .sendId = sendId,
            .historyEntryId = reservation->entryId,
            .connectionId = std::move(connectionId),
            .accountId = prepared.draft.accountId,
            .composeSessionId = prepared.draft.composeSessionId,
            .draftEmailId = prepared.draft.draftEmailId,
            .subject = snapshot.subject,
            .status = DeferredSendStatus::Scheduled,
            .dueAt = dueAt,
            .dispatchStartedAt = std::nullopt,
            .submissionId = std::nullopt,
            .lastError = std::nullopt,
            .createdAt = {},
            .updatedAt = {},
        };
        if (const auto error = m_repository.insertAndActivateHistory(send))
        {
            static_cast<void>(m_undoManager.discardNormal(reservation->entryId));
            co_return javelin::jmap::operationError(*error);
        }
        static_cast<void>(m_undoManager.load());

        const auto timeout =
            std::clamp<std::int64_t>(delay.count() * 1000, 1, std::numeric_limits<int>::max());
        Q_EMIT undoableSendScheduled(sendId, QStringLiteral("Message scheduled"),
                                     sendLabel(snapshot.subject), static_cast<int>(timeout));
        scheduleNext();
        co_return javelin::jmap::submission::SendSummary{
            .composeSessionId = prepared.draft.composeSessionId,
            .accountId = prepared.draft.accountId,
            .draftEmailId = prepared.draft.draftEmailId,
            .submissionId = std::nullopt,
            .scheduled = true,
        };
    }

    std::variant<bool, javelin::jmap::OperationError>
    DeferredSendService::cancelTargeted(const QString& sendId)
    {
        if (m_undoManager.state().executing)
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::Conflict,
                .message =
                    QStringLiteral("Unable to cancel a scheduled send while history is changing."),
            };
        const auto found = m_repository.find(sendId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            return javelin::jmap::operationError(*error);
        const auto& send = std::get<std::optional<PendingSend>>(found);
        if (!send.has_value())
            return false;
        const auto cancelled = m_repository.cancelBeforeDispatch(sendId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cancelled))
            return javelin::jmap::operationError(*error);
        if (!std::get<bool>(cancelled))
            return false;
        if (const auto error = m_undoManager.forgetAndClearRedo(send->historyEntryId))
            return javelin::jmap::operationError(*error);
        Q_EMIT undoableSendClosed(sendId);
        Q_EMIT draftRestoreRequested(QString::fromStdString(send->accountId),
                                     QString::fromStdString(send->draftEmailId),
                                     QString::fromStdString(send->composeSessionId));
        scheduleNext();
        return true;
    }

    QCoro::Task<javelin::app::undo::HistoryExecutionResult>
    DeferredSendService::execute(javelin::app::undo::HistoryEntry entry,
                                 const javelin::app::undo::HistoryExecutionDirection direction)
    {
        auto* history = std::get_if<javelin::app::undo::DeferredSendHistory>(&entry.payload);
        if (history == nullptr ||
            direction == javelin::app::undo::HistoryExecutionDirection::Recover)
            co_return historyFailure(QStringLiteral("The scheduled send requires reconciliation."),
                                     javelin::app::undo::HistoryExecutionOutcome::Unknown);

        const auto sendId = QString::fromStdString(history->sendId);
        if (direction == javelin::app::undo::HistoryExecutionDirection::Undo)
        {
            const auto cancelled = m_repository.cancelBeforeDispatch(sendId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cancelled))
                co_return historyFailure(
                    error->message, javelin::app::undo::HistoryExecutionOutcome::DefinitiveFailure);
            if (!std::get<bool>(cancelled))
                co_return historyFailure(QStringLiteral("The message has already started sending."),
                                         javelin::app::undo::HistoryExecutionOutcome::Expired);
            Q_EMIT undoableSendClosed(sendId);
            Q_EMIT draftRestoreRequested(QString::fromStdString(history->accountId),
                                         QString::fromStdString(history->draftEmailId),
                                         QString::fromStdString(history->composeSessionId));
        }
        else
        {
            const auto dueAt = now().addSecs(history->delaySeconds);
            const auto rescheduled = m_repository.reschedule(sendId, dueAt);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&rescheduled))
                co_return historyFailure(
                    error->message, javelin::app::undo::HistoryExecutionOutcome::DefinitiveFailure);
            if (!std::get<bool>(rescheduled))
                co_return historyFailure(
                    QStringLiteral("The scheduled send can no longer be redone."),
                    javelin::app::undo::HistoryExecutionOutcome::Conflict);
            Q_EMIT undoableSendScheduled(sendId, QStringLiteral("Message scheduled"),
                                         sendLabel(history->subject),
                                         static_cast<int>(history->delaySeconds * 1000));
        }
        scheduleNext();
        co_return historySuccess(*history);
    }

    void DeferredSendService::scheduleNext()
    {
        if (m_dispatchRunning)
            return;
        const auto listed = m_repository.listRecoverable();
        if (!std::holds_alternative<std::vector<PendingSend>>(listed))
            return;
        const auto& sends = std::get<std::vector<PendingSend>>(listed);
        const auto next =
            std::ranges::find_if(sends, [](const PendingSend& send)
                                 { return send.status != DeferredSendStatus::Unknown; });
        if (next == sends.end())
        {
            m_timer.stop();
            return;
        }
        auto delay = now().msecsTo(next->dueAt);
        if (next->status == DeferredSendStatus::WaitingForNetwork ||
            next->status == DeferredSendStatus::WaitingForAuth)
            delay = std::max<qint64>(delay, 30000);
        m_timer.start(
            static_cast<int>(std::clamp<qint64>(delay, 0, std::numeric_limits<int>::max())));
    }

    void DeferredSendService::dispatchDue()
    {
        if (m_dispatchRunning)
            return;
        const auto listed = m_repository.listRecoverable();
        if (!std::holds_alternative<std::vector<PendingSend>>(listed))
            return;
        const auto& sends = std::get<std::vector<PendingSend>>(listed);
        const auto due = std::ranges::find_if(
            sends, [&](const PendingSend& send)
            { return send.status != DeferredSendStatus::Unknown && send.dueAt <= now(); });
        if (due == sends.end())
        {
            scheduleNext();
            return;
        }
        m_dispatchRunning = true;
        QCoro::connect(dispatch(*due), this,
                       [this]()
                       {
                           m_dispatchRunning = false;
                           scheduleNext();
                       });
    }

    QCoro::Task<void> DeferredSendService::dispatch(PendingSend send)
    {
        const auto settings = m_connectionProvider.connectionSettingsFor(send.accountId);
        if (!settings.has_value())
        {
            static_cast<void>(
                m_repository.markWaiting(send.sendId, DeferredSendStatus::WaitingForAuth,
                                         QStringLiteral("Account credentials are unavailable.")));
            Q_EMIT undoableSendWaiting(send.sendId, QStringLiteral("Waiting to send"),
                                       sendLabel(send.subject));
            co_return;
        }
        const auto claimed = m_repository.claimForDispatch(send.sendId, now());
        if (!std::holds_alternative<bool>(claimed) || !std::get<bool>(claimed))
            co_return;

        bool dispatched = false;
        auto submitted = co_await m_composeService.submitPreparedSend(
            liveSettings(*settings),
            {.draft =
                 {
                     .composeSessionId = send.composeSessionId,
                     .accountId = send.accountId,
                     .draftEmailId = send.draftEmailId,
                     .operationGroupId = {},
                     .createMutationId = {},
                     .destroyMutationId = std::nullopt,
                     .savedSnapshot = {},
                 }},
            [this, &dispatched, send]()
            {
                dispatched = true;
                const auto explanation =
                    QStringLiteral(
                        "Unable to undo sending %1 because it has already been submitted.")
                        .arg(send.subject.has_value()
                                 ? QStringLiteral("“%1”").arg(QString::fromStdString(*send.subject))
                                 : QStringLiteral("this message"));
                static_cast<void>(m_undoManager.setEntryStatus(
                    send.historyEntryId, javelin::app::undo::HistoryEntryStatus::Expired,
                    explanation));
                Q_EMIT undoableSendClosed(send.sendId);
            });
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitted))
        {
            if (!dispatched &&
                (error->code == javelin::jmap::OperationErrorCode::AuthenticationRequired ||
                 error->code == javelin::jmap::OperationErrorCode::NetworkUnavailable))
            {
                const auto waiting =
                    error->code == javelin::jmap::OperationErrorCode::AuthenticationRequired
                        ? DeferredSendStatus::WaitingForAuth
                        : DeferredSendStatus::WaitingForNetwork;
                static_cast<void>(m_repository.markWaiting(send.sendId, waiting, error->message));
                static_cast<void>(m_undoManager.setEntryStatus(
                    send.historyEntryId, javelin::app::undo::HistoryEntryStatus::Ready));
                Q_EMIT undoableSendWaiting(send.sendId, QStringLiteral("Waiting to send"),
                                           sendLabel(send.subject));
            }
            else if (dispatched &&
                     (javelin::jmap::isTransientError(*error) ||
                      error->code == javelin::jmap::OperationErrorCode::Cancelled ||
                      error->code == javelin::jmap::OperationErrorCode::ProtocolViolation))
                static_cast<void>(m_repository.markUnknown(send.sendId, error->message));
            else
            {
                static_cast<void>(m_repository.markFailed(send.sendId, error->message));
                static_cast<void>(m_undoManager.forget(send.historyEntryId));
                Q_EMIT sendFailed(send.sendId, error->message);
            }
            co_return;
        }
        const auto& summary = std::get<javelin::jmap::submission::SendSummary>(submitted);
        static_cast<void>(m_repository.markSubmitted(send.sendId, summary.submissionId));
    }

    QDateTime DeferredSendService::now() const
    {
        return m_clock ? m_clock().toUTC() : QDateTime::currentDateTimeUtc();
    }
} // namespace javelin::app
