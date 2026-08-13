#include "client/RemoteActionClient.h"

#include "app/PerformanceMetrics.h"
#include "client/GuiDaemonSession.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QUuid>

#include <utility>
#include <vector>

namespace javelin::app
{
    namespace
    {
        constexpr auto messageListWindowTerminalTimeout = std::chrono::seconds{90};

        [[nodiscard]] QString remoteActionMetricName(const javelin::protocol::ActionId action)
        {
            return PerformanceMetrics::remoteActionName(
                javelin::protocol::actions::actionName(action));
        }
    } // namespace

    RemoteActionClient::RemoteActionClient(GuiDaemonSession& session, QObject* parent)
        : QObject(parent), m_session(session)
    {
        connect(&m_session, &GuiDaemonSession::operationCompleted, this,
                &RemoteActionClient::complete);
        connect(&m_session, &GuiDaemonSession::operationFailed, this, &RemoteActionClient::fail);
        connect(&m_session, &GuiDaemonSession::recoveryFinished, this,
                &RemoteActionClient::retryPending);
        connect(&m_session, &GuiDaemonSession::daemonShutdownRequested, this,
                [this] { failAll(QStringLiteral("The Javelin daemon is shutting down.")); });
    }

    RemoteActionClient::~RemoteActionClient()
    {
        failAll(QStringLiteral("The GUI remote action client was destroyed."));
    }

    RemoteActionClient::RawResult
    RemoteActionClient::invokeImmediate(const javelin::protocol::ActionId action,
                                        QByteArray payload)
    {
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("remote_action_e2e"),
                                QStringLiteral("kind=%1 payload_bytes=%2")
                                    .arg(remoteActionMetricName(action))
                                    .arg(payload.size())};
        const auto commandId = javelin::protocol::CommandId{.value = QUuid::createUuid()};
        const auto originalDaemon = m_session.daemonInstance();
        auto reply = m_session.submitRemoteAction(action, payload, commandId);
        if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply);
            rejected != nullptr &&
            rejected->error.code == javelin::protocol::BoundaryErrorCode::TransportUnavailable)
        {
            if (!originalDaemon.has_value())
            {
                metrics.finish(QStringLiteral("transport_unavailable"));
                return error(rejected->error);
            }
            if (const auto reconnectError = m_session.reconnect())
            {
                metrics.finish(QStringLiteral("transport_unavailable"));
                return RemoteCallError{
                    .code = javelin::protocol::BoundaryErrorCode::TransportUnavailable,
                    .detail = reconnectError->detail,
                };
            }
            const auto reconnectedDaemon = m_session.daemonInstance();
            if (!reconnectedDaemon.has_value() || *reconnectedDaemon != *originalDaemon)
            {
                metrics.finish(QStringLiteral("unknown"));
                return RemoteCallError{
                    .code = javelin::protocol::BoundaryErrorCode::TransportUnavailable,
                    .detail = QStringLiteral(
                        "The daemon restarted before the command result was known; the command was "
                        "not repeated automatically."),
                };
            }
            reply = m_session.submitRemoteAction(action, std::move(payload), commandId);
        }

        if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply))
        {
            metrics.finish(QStringLiteral("rejected"));
            scheduleAcknowledgement(commandId);
            return error(rejected->error);
        }
        const auto& accepted = std::get<javelin::protocol::CommandAccepted>(reply);
        if (!accepted.immediateResult.has_value())
        {
            metrics.finish(QStringLiteral("protocol_error"));
            return RemoteCallError{
                .code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                .detail = QStringLiteral("The daemon did not return an immediate result."),
            };
        }
        metrics.finish(QStringLiteral("completed"));
        auto result = *accepted.immediateResult;
        scheduleAcknowledgement(commandId);
        return result;
    }

    QFuture<RemoteActionClient::RawResult>
    RemoteActionClient::invoke(const javelin::protocol::ActionId action, QByteArray payload)
    {
        const auto commandId = javelin::protocol::CommandId{.value = QUuid::createUuid()};
        auto pending = std::make_unique<PendingCall>();
        pending->promise.start();
        auto future = pending->promise.future();
        pending->commandId = commandId;
        pending->action = action;
        pending->payload = std::move(payload);
        pending->daemon = m_session.daemonInstance();
        pending->startedAt = std::chrono::steady_clock::now();
        const auto pendingKey = key(commandId.value);
        m_pending.emplace(pendingKey, std::move(pending));
        submitPending(pendingKey);
        return future;
    }

    void RemoteActionClient::submitPending(const QString& pendingKey)
    {
        const auto found = m_pending.find(pendingKey);
        if (found == m_pending.end() || found->second->submissionInFlight || !m_session.isReady())
            return;

        auto& pending = *found->second;
        const auto daemon = m_session.daemonInstance();
        if (pending.daemon.has_value() && daemon.has_value() && *pending.daemon != *daemon)
        {
            fail(javelin::protocol::OperationId{.value = pending.commandId.value},
                 {.code = javelin::protocol::BoundaryErrorCode::TransportUnavailable,
                  .field = QStringLiteral("daemon.instance"),
                  .detail = QStringLiteral("The daemon restarted before the command result was "
                                           "known; the command was not "
                                           "repeated automatically.")});
            return;
        }
        if (!pending.daemon.has_value())
            pending.daemon = daemon;

        pending.submissionInFlight = true;
        pending.submissionStartedAt = std::chrono::steady_clock::now();
        const auto commandId = pending.commandId;
        auto* watcher = new QFutureWatcher<javelin::protocol::CommandReply>(this);
        connect(
            watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, pendingKey, commandId]
            {
                const auto reply = watcher->result();
                watcher->deleteLater();
                const auto pendingCall = m_pending.find(pendingKey);
                if (pendingCall == m_pending.end())
                    return;
                pendingCall->second->submissionInFlight = false;
                const auto recordAdmission = [&](const QString& outcome)
                {
                    if (!pendingCall->second->submissionStartedAt.has_value())
                        return;
                    PerformanceMetrics::recordDuration(
                        QStringLiteral("gui"), QStringLiteral("remote_action_admission"),
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() -
                            *pendingCall->second->submissionStartedAt),
                        outcome,
                        QStringLiteral("kind=%1 payload_bytes=%2")
                            .arg(remoteActionMetricName(pendingCall->second->action))
                            .arg(pendingCall->second->payload.size()));
                    pendingCall->second->submissionStartedAt.reset();
                };

                if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply))
                {
                    if (rejected->error.code ==
                        javelin::protocol::BoundaryErrorCode::TransportUnavailable)
                    {
                        recordAdmission(QStringLiteral("transport_unavailable"));
                        if (m_session.isReady())
                            QTimer::singleShot(0, this,
                                               [this, pendingKey] { submitPending(pendingKey); });
                        return;
                    }
                    recordAdmission(QStringLiteral("rejected"));
                    fail(javelin::protocol::OperationId{.value = commandId.value}, rejected->error);
                    return;
                }

                const auto& accepted = std::get<javelin::protocol::CommandAccepted>(reply);
                recordAdmission(QStringLiteral("accepted"));
                if (accepted.immediateResult.has_value())
                {
                    complete(javelin::protocol::OperationId{.value = commandId.value},
                             *accepted.immediateResult);
                    return;
                }
                if (!accepted.operation.has_value() || accepted.operation->value != commandId.value)
                {
                    fail(javelin::protocol::OperationId{.value = commandId.value},
                         {.code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                          .field = QStringLiteral("command.operation"),
                          .detail = QStringLiteral(
                              "The daemon returned an invalid operation identifier.")});
                    return;
                }
                const bool messageListWindow =
                    pendingCall->second->action == javelin::protocol::actions::MailboxWindow::id ||
                    pendingCall->second->action == javelin::protocol::actions::SearchWindow::id;
                if (messageListWindow && !pendingCall->second->terminalTimeoutScheduled)
                {
                    pendingCall->second->terminalTimeoutScheduled = true;
                    QTimer::singleShot(
                        messageListWindowTerminalTimeout, this,
                        [this, pendingKey, commandId]
                        {
                            if (!m_pending.contains(pendingKey))
                                return;
                            fail(
                                javelin::protocol::OperationId{.value = commandId.value},
                                {.code = javelin::protocol::BoundaryErrorCode::TransportUnavailable,
                                 .field = QStringLiteral("command.operation"),
                                 .detail = QStringLiteral("The message-list request did not "
                                                          "produce a terminal result.")});
                        });
                }
            });
        watcher->setFuture(
            m_session.submitRemoteActionAsync(pending.action, pending.payload, pending.commandId));
    }

    void RemoteActionClient::retryPending()
    {
        std::vector<QString> keys;
        keys.reserve(m_pending.size());
        for (const auto& [pendingKey, pending] : m_pending)
        {
            Q_UNUSED(pending)
            keys.push_back(pendingKey);
        }
        for (const auto& pendingKey : keys)
            submitPending(pendingKey);

        keys.clear();
        keys.reserve(m_pendingAcknowledgements.size());
        for (const auto& acknowledgedKey : m_pendingAcknowledgements)
            keys.push_back(acknowledgedKey);
        for (const auto& acknowledgedKey : keys)
            submitAcknowledgement(acknowledgedKey);
    }

    void RemoteActionClient::scheduleAcknowledgement(const javelin::protocol::CommandId& commandId)
    {
        const auto acknowledgedKey = key(commandId.value);
        m_pendingAcknowledgements.insert(acknowledgedKey);
        QTimer::singleShot(0, this,
                           [this, acknowledgedKey] { submitAcknowledgement(acknowledgedKey); });
    }

    void RemoteActionClient::submitAcknowledgement(const QString& acknowledgedKey)
    {
        if (!m_session.isReady() || !m_pendingAcknowledgements.contains(acknowledgedKey) ||
            m_acknowledgementsInFlight.contains(acknowledgedKey))
            return;

        auto encoded = remote::encodeVersioned<
            javelin::protocol::actions::AcknowledgeRemoteActionResult::requestSchemaVersion>(
            acknowledgedKey);
        if (std::holds_alternative<remote::CodecError>(encoded))
        {
            m_pendingAcknowledgements.erase(acknowledgedKey);
            return;
        }

        m_acknowledgementsInFlight.insert(acknowledgedKey);
        auto* watcher = new QFutureWatcher<javelin::protocol::CommandReply>(this);
        connect(watcher, &QFutureWatcherBase::finished, this,
                [this, watcher, acknowledgedKey]
                {
                    const auto reply = watcher->result();
                    watcher->deleteLater();
                    m_acknowledgementsInFlight.erase(acknowledgedKey);
                    if (std::holds_alternative<javelin::protocol::CommandAccepted>(reply))
                    {
                        m_pendingAcknowledgements.erase(acknowledgedKey);
                        return;
                    }
                    const auto& rejected = std::get<javelin::protocol::CommandRejected>(reply);
                    if (rejected.error.code !=
                            javelin::protocol::BoundaryErrorCode::TransportUnavailable &&
                        rejected.error.code != javelin::protocol::BoundaryErrorCode::Busy)
                    {
                        m_pendingAcknowledgements.erase(acknowledgedKey);
                    }
                });
        watcher->setFuture(m_session.submitRemoteActionAsync(
            javelin::protocol::actions::AcknowledgeRemoteActionResult::id,
            std::get<QByteArray>(std::move(encoded))));
    }

    void RemoteActionClient::complete(const javelin::protocol::OperationId& operation,
                                      QByteArray result)
    {
        scheduleAcknowledgement({.value = operation.value});
        const auto found = m_pending.find(key(operation.value));
        if (found == m_pending.end())
            return;
        PerformanceMetrics::recordDuration(
            QStringLiteral("gui"), QStringLiteral("remote_action_e2e"),
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  found->second->startedAt),
            QStringLiteral("completed"),
            QStringLiteral("kind=%1 payload_bytes=%2")
                .arg(remoteActionMetricName(found->second->action))
                .arg(found->second->payload.size()));
        found->second->promise.addResult(RawResult{std::move(result)});
        found->second->promise.finish();
        m_pending.erase(found);
    }

    void RemoteActionClient::fail(const javelin::protocol::OperationId& operation,
                                  const javelin::protocol::BoundaryError& boundaryError)
    {
        scheduleAcknowledgement({.value = operation.value});
        const auto found = m_pending.find(key(operation.value));
        if (found == m_pending.end())
            return;
        PerformanceMetrics::recordDuration(
            QStringLiteral("gui"), QStringLiteral("remote_action_e2e"),
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                  found->second->startedAt),
            QStringLiteral("failed"),
            QStringLiteral("kind=%1 payload_bytes=%2 code=%3")
                .arg(remoteActionMetricName(found->second->action))
                .arg(found->second->payload.size())
                .arg(static_cast<int>(boundaryError.code)));
        found->second->promise.addResult(RawResult{error(boundaryError)});
        found->second->promise.finish();
        m_pending.erase(found);
    }

    void RemoteActionClient::failAll(QString detail)
    {
        auto pending = std::move(m_pending);
        m_pending.clear();
        m_pendingAcknowledgements.clear();
        m_acknowledgementsInFlight.clear();
        for (auto& [pendingKey, call] : pending)
        {
            Q_UNUSED(pendingKey)
            PerformanceMetrics::recordDuration(
                QStringLiteral("gui"), QStringLiteral("remote_action_e2e"),
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - call->startedAt),
                QStringLiteral("abandoned"),
                QStringLiteral("kind=%1 payload_bytes=%2")
                    .arg(remoteActionMetricName(call->action))
                    .arg(call->payload.size()));
            call->promise.addResult(RawResult{RemoteCallError{.detail = detail}});
            call->promise.finish();
        }
    }

    QString RemoteActionClient::key(const QUuid& id)
    {
        return id.toString(QUuid::WithoutBraces);
    }

    RemoteCallError RemoteActionClient::error(const javelin::protocol::BoundaryError& boundaryError)
    {
        return {.code = boundaryError.code, .detail = boundaryError.detail};
    }
} // namespace javelin::app
