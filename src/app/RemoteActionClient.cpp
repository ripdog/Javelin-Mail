#include "app/RemoteActionClient.h"

#include "app/GuiDaemonSession.h"

#include <QFutureWatcher>
#include <QTimer>
#include <QUuid>

#include <utility>
#include <vector>

namespace javelin::app
{
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
    RemoteActionClient::invokeImmediate(const javelin::protocol::RemoteActionKind kind,
                                        QByteArray payload)
    {
        const auto commandId = javelin::protocol::CommandId{.value = QUuid::createUuid()};
        const auto originalDaemon = m_session.daemonInstance();
        auto reply = m_session.submitRemoteAction(kind, payload, commandId);
        if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply);
            rejected != nullptr &&
            rejected->error.code == javelin::protocol::BoundaryErrorCode::TransportUnavailable)
        {
            if (!originalDaemon.has_value())
                return error(rejected->error);
            if (const auto reconnectError = m_session.reconnect())
            {
                return RemoteCallError{
                    .code = javelin::protocol::BoundaryErrorCode::TransportUnavailable,
                    .detail = reconnectError->detail,
                };
            }
            const auto reconnectedDaemon = m_session.daemonInstance();
            if (!reconnectedDaemon.has_value() || *reconnectedDaemon != *originalDaemon)
            {
                return RemoteCallError{
                    .code = javelin::protocol::BoundaryErrorCode::TransportUnavailable,
                    .detail = QStringLiteral(
                        "The daemon restarted before the command result was known; the command was "
                        "not repeated automatically."),
                };
            }
            reply = m_session.submitRemoteAction(kind, std::move(payload), commandId);
        }

        if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply))
            return error(rejected->error);
        const auto& accepted = std::get<javelin::protocol::CommandAccepted>(reply);
        if (!accepted.immediateResult.has_value())
        {
            return RemoteCallError{
                .code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                .detail = QStringLiteral("The daemon did not return an immediate result."),
            };
        }
        return *accepted.immediateResult;
    }

    QFuture<RemoteActionClient::RawResult>
    RemoteActionClient::invoke(const javelin::protocol::RemoteActionKind kind, QByteArray payload)
    {
        const auto commandId = javelin::protocol::CommandId{.value = QUuid::createUuid()};
        auto pending = std::make_unique<PendingCall>();
        pending->promise.start();
        auto future = pending->promise.future();
        pending->commandId = commandId;
        pending->kind = kind;
        pending->payload = std::move(payload);
        pending->daemon = m_session.daemonInstance();
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

                if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply))
                {
                    if (rejected->error.code ==
                        javelin::protocol::BoundaryErrorCode::TransportUnavailable)
                    {
                        if (m_session.isReady())
                            QTimer::singleShot(0, this,
                                               [this, pendingKey] { submitPending(pendingKey); });
                        return;
                    }
                    fail(javelin::protocol::OperationId{.value = commandId.value}, rejected->error);
                    return;
                }

                const auto& accepted = std::get<javelin::protocol::CommandAccepted>(reply);
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
                }
            });
        watcher->setFuture(
            m_session.submitRemoteActionAsync(pending.kind, pending.payload, pending.commandId));
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
    }

    void RemoteActionClient::complete(const javelin::protocol::OperationId& operation,
                                      QByteArray result)
    {
        const auto found = m_pending.find(key(operation.value));
        if (found == m_pending.end())
            return;
        found->second->promise.addResult(RawResult{std::move(result)});
        found->second->promise.finish();
        m_pending.erase(found);
    }

    void RemoteActionClient::fail(const javelin::protocol::OperationId& operation,
                                  const javelin::protocol::BoundaryError& boundaryError)
    {
        const auto found = m_pending.find(key(operation.value));
        if (found == m_pending.end())
            return;
        found->second->promise.addResult(RawResult{error(boundaryError)});
        found->second->promise.finish();
        m_pending.erase(found);
    }

    void RemoteActionClient::failAll(QString detail)
    {
        auto pending = std::move(m_pending);
        m_pending.clear();
        for (auto& [pendingKey, call] : pending)
        {
            Q_UNUSED(pendingKey)
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
