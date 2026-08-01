#include "app/RemoteActionClient.h"

#include "app/GuiDaemonSession.h"

#include <QUuid>

#include <utility>

namespace javelin::app
{
    RemoteActionClient::RemoteActionClient(GuiDaemonSession& session, QObject* parent)
        : QObject(parent), m_session(session)
    {
        connect(&m_session, &GuiDaemonSession::operationCompleted, this,
                &RemoteActionClient::complete);
        connect(&m_session, &GuiDaemonSession::operationFailed, this, &RemoteActionClient::fail);
        connect(&m_session, &GuiDaemonSession::recoveryStarted, this,
                [this](const QString& detail) { failAll(detail); });
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
        const auto reply = m_session.submitRemoteAction(kind, std::move(payload));
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
        const auto pendingKey = key(commandId.value);
        m_pending.emplace(pendingKey, std::move(pending));

        const auto reply = m_session.submitRemoteAction(kind, std::move(payload), commandId);
        if (const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply))
        {
            fail(javelin::protocol::OperationId{.value = commandId.value}, rejected->error);
            return future;
        }

        const auto& accepted = std::get<javelin::protocol::CommandAccepted>(reply);
        if (accepted.immediateResult.has_value())
        {
            complete(javelin::protocol::OperationId{.value = commandId.value},
                     *accepted.immediateResult);
            return future;
        }
        if (!accepted.operation.has_value() || accepted.operation->value != commandId.value)
        {
            fail(
                javelin::protocol::OperationId{.value = commandId.value},
                {.code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                 .field = QStringLiteral("command.operation"),
                 .detail = QStringLiteral("The daemon returned an invalid operation identifier.")});
        }
        return future;
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
