#pragma once

#include "app/RemoteCodec.h"
#include "protocol/ActionContract.h"
#include "protocol/HandshakeContract.h"
#include "protocol/actions/ActionCatalog.h"

#include <QFuture>
#include <QObject>
#include <QPromise>

#include <QCoroFuture>
#include <QCoroTask>

#include <chrono>
#include <concepts>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

        [[nodiscard]] RawResult invokeImmediate(javelin::protocol::ActionId action,
                                                QByteArray payload);
        [[nodiscard]] QFuture<RawResult> invoke(javelin::protocol::ActionId action,
                                                QByteArray payload);

        template <typename Action, typename... Arguments>
        [[nodiscard]] DecodedRemoteResult<typename Action::Result>
        callImmediate(const Arguments&... arguments)
        {
            static_assert(std::same_as<typename Action::Request,
                                       std::tuple<std::remove_cvref_t<Arguments>...>>,
                          "Remote action arguments do not match the descriptor request type");
            auto encoded = remote::encodeVersioned<Action::requestSchemaVersion>(arguments...);
            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                return RemoteCallError{.detail = error->message};
            auto raw = invokeImmediate(Action::id, std::get<QByteArray>(std::move(encoded)));
            if (const auto* error = std::get_if<RemoteCallError>(&raw))
                return *error;
            using Result = typename Action::Result;
            auto decoded = remote::decodeVersionedValue<Action::resultSchemaVersion, Result>(
                std::get<QByteArray>(raw));
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                return RemoteCallError{.code =
                                           javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                                       .detail = error->message};
            return std::get<Result>(std::move(decoded));
        }

        template <typename Action, typename... Arguments>
        [[nodiscard]] QCoro::Task<DecodedRemoteResult<typename Action::Result>>
        call(const Arguments&... arguments)
        {
            static_assert(std::same_as<typename Action::Request,
                                       std::tuple<std::remove_cvref_t<Arguments>...>>,
                          "Remote action arguments do not match the descriptor request type");
            auto encoded = remote::encodeVersioned<Action::requestSchemaVersion>(arguments...);
            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                co_return RemoteCallError{.detail = error->message};
            auto future = invoke(Action::id, std::get<QByteArray>(std::move(encoded)));
            auto raw = co_await qCoro(future).takeResult();
            if (const auto* error = std::get_if<RemoteCallError>(&raw))
                co_return *error;
            using Result = typename Action::Result;
            auto decoded = remote::decodeVersionedValue<Action::resultSchemaVersion, Result>(
                std::get<QByteArray>(raw));
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                co_return RemoteCallError{
                    .code = javelin::protocol::BoundaryErrorCode::ProtocolViolation,
                    .detail = error->message};
            co_return std::get<Result>(std::move(decoded));
        }

        template <typename Action, typename... Arguments>
        [[nodiscard]] QCoro::Task<bool> callDiscardingResult(const Arguments&... arguments)
        {
            static_assert(std::same_as<typename Action::Request,
                                       std::tuple<std::remove_cvref_t<Arguments>...>>,
                          "Remote action arguments do not match the descriptor request type");
            auto encoded = remote::encodeVersioned<Action::requestSchemaVersion>(arguments...);
            if (std::holds_alternative<remote::CodecError>(encoded))
                co_return false;
            auto future = invoke(Action::id, std::get<QByteArray>(std::move(encoded)));
            auto raw = co_await qCoro(future).takeResult();
            co_return std::holds_alternative<QByteArray>(raw);
        }

      private:
        struct PendingCall
        {
            QPromise<RawResult> promise;
            javelin::protocol::CommandId commandId;
            javelin::protocol::ActionId action;
            QByteArray payload;
            std::optional<javelin::protocol::DaemonInstanceId> daemon;
            std::chrono::steady_clock::time_point startedAt;
            std::optional<std::chrono::steady_clock::time_point> submissionStartedAt;
            bool submissionInFlight = false;
            bool terminalTimeoutScheduled = false;
        };

        void submitPending(const QString& pendingKey);
        void retryPending();
        void scheduleAcknowledgement(const javelin::protocol::CommandId& commandId);
        void submitAcknowledgement(const QString& acknowledgedKey);
        void complete(const javelin::protocol::OperationId& operation, QByteArray result);
        void fail(const javelin::protocol::OperationId& operation,
                  const javelin::protocol::BoundaryError& error);
        void failAll(QString detail);
        [[nodiscard]] static QString key(const QUuid& id);
        [[nodiscard]] static RemoteCallError error(const javelin::protocol::BoundaryError& error);

        GuiDaemonSession& m_session;
        std::unordered_map<QString, std::unique_ptr<PendingCall>> m_pending;
        std::unordered_set<QString> m_pendingAcknowledgements;
        std::unordered_set<QString> m_acknowledgementsInFlight;
    };
} // namespace javelin::app
