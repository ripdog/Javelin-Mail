#pragma once

#include "app/RemoteCodec.h"
#include "protocol/ProcessBoundary.h"

#include <QFuture>
#include <QObject>
#include <QPromise>

#include <QCoroFuture>
#include <QCoroTask>

#include <memory>
#include <unordered_map>
#include <variant>

namespace javelin::app
{
    class GuiDaemonSession;

    struct RemoteCallError
    {
        javelin::protocol::BoundaryErrorCode code =
            javelin::protocol::BoundaryErrorCode::TransportUnavailable;
        QString detail;
    };

    template <typename Result> using DecodedRemoteResult = std::variant<Result, RemoteCallError>;

    class RemoteActionClient final : public QObject
    {
        Q_OBJECT

      public:
        using RawResult = std::variant<QByteArray, RemoteCallError>;

        explicit RemoteActionClient(GuiDaemonSession& session, QObject* parent = nullptr);
        ~RemoteActionClient() override;

        [[nodiscard]] RawResult invokeImmediate(javelin::protocol::RemoteActionKind kind,
                                                QByteArray payload);
        [[nodiscard]] QFuture<RawResult> invoke(javelin::protocol::RemoteActionKind kind,
                                                QByteArray payload);

        template <typename Result, typename... Arguments>
        [[nodiscard]] DecodedRemoteResult<Result>
        callImmediate(const javelin::protocol::RemoteActionKind kind, const Arguments&... arguments)
        {
            auto encoded = remote::encode(arguments...);
            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                return RemoteCallError{.detail = error->message};
            auto raw = invokeImmediate(kind, std::get<QByteArray>(std::move(encoded)));
            if (const auto* error = std::get_if<RemoteCallError>(&raw))
                return *error;
            auto decoded = remote::decodeValue<Result>(std::get<QByteArray>(raw));
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                return RemoteCallError{.code =
                                           javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                                       .detail = error->message};
            return std::get<Result>(std::move(decoded));
        }

        template <typename Result, typename... Arguments>
        [[nodiscard]] QCoro::Task<DecodedRemoteResult<Result>>
        call(const javelin::protocol::RemoteActionKind kind, const Arguments&... arguments)
        {
            auto encoded = remote::encode(arguments...);
            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                co_return RemoteCallError{.detail = error->message};
            auto future = invoke(kind, std::get<QByteArray>(std::move(encoded)));
            auto raw = co_await qCoro(future).takeResult();
            if (const auto* error = std::get_if<RemoteCallError>(&raw))
                co_return *error;
            auto decoded = remote::decodeValue<Result>(std::get<QByteArray>(raw));
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                co_return RemoteCallError{
                    .code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                    .detail = error->message};
            co_return std::get<Result>(std::move(decoded));
        }

        template <typename... Arguments>
        [[nodiscard]] QCoro::Task<bool>
        callDiscardingResult(const javelin::protocol::RemoteActionKind kind,
                             const Arguments&... arguments)
        {
            auto encoded = remote::encode(arguments...);
            if (std::holds_alternative<remote::CodecError>(encoded))
                co_return false;
            auto future = invoke(kind, std::get<QByteArray>(std::move(encoded)));
            auto raw = co_await qCoro(future).takeResult();
            co_return std::holds_alternative<QByteArray>(raw);
        }

      private:
        struct PendingCall
        {
            QPromise<RawResult> promise;
            javelin::protocol::CommandId commandId;
            javelin::protocol::RemoteActionKind kind;
            QByteArray payload;
            std::optional<javelin::protocol::DaemonInstanceId> daemon;
            bool submissionInFlight = false;
        };

        void submitPending(const QString& pendingKey);
        void retryPending();
        void complete(const javelin::protocol::OperationId& operation, QByteArray result);
        void fail(const javelin::protocol::OperationId& operation,
                  const javelin::protocol::BoundaryError& error);
        void failAll(QString detail);
        [[nodiscard]] static QString key(const QUuid& id);
        [[nodiscard]] static RemoteCallError error(const javelin::protocol::BoundaryError& error);

        GuiDaemonSession& m_session;
        std::unordered_map<QString, std::unique_ptr<PendingCall>> m_pending;
    };
} // namespace javelin::app
