#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/OnboardingTypes.h"
#include "app/RemoteActionTypes.h"
#include "app/RemoteCodec.h"
#include "protocol/ActionContract.h"
#include "protocol/BoundaryEventContract.h"
#include "protocol/actions/ActionCatalog.h"

#include <QCoroTask>

#include <QObject>
#include <QTimer>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace javelin::app
{
    class DaemonServices;
    class MailboxObservation;

    class DaemonRemoteActionDispatcher final : public QObject
    {
        Q_OBJECT

      public:
        using AuthenticationResultFilter =
            std::function<AccountAuthenticationResult(AccountAuthenticationResult)>;
        using ConnectionSettingsHydrator =
            std::function<std::variant<AccountConnectionSettings, QString>(
                AccountConnectionSettings)>;
        using RevocationRequestHydrator =
            std::function<std::variant<OAuthRevocationRequest, QString>(OAuthRevocationRequest)>;

        DaemonRemoteActionDispatcher(
            DaemonServices& services, javelin::protocol::BoundaryEventSink& eventSink,
            std::function<javelin::protocol::InvalidationEpoch()> currentEpoch,
            std::function<std::optional<javelin::protocol::BoundaryError>()> reloadSettings,
            AuthenticationResultFilter authenticationResultFilter,
            ConnectionSettingsHydrator connectionSettingsHydrator,
            RevocationRequestHydrator revocationRequestHydrator, QObject* parent = nullptr);
        ~DaemonRemoteActionDispatcher() override;

        [[nodiscard]] javelin::protocol::CommandReply
        dispatch(javelin::protocol::CommandRequest request);
        void releaseGuiResources();

      private:
        struct ReplayEntry
        {
            javelin::protocol::CommandId id;
            javelin::protocol::ActionId action{};
            QByteArray payloadDigest;
            std::optional<javelin::protocol::CommandReply> reply;
            bool pending = false;
            bool repeatable = false;
            bool terminalResultExpired = false;
            std::size_t retainedResultBytes = 0;
            std::chrono::steady_clock::time_point startedAt;
            std::optional<std::chrono::steady_clock::time_point> completedAt;
        };

        [[nodiscard]] javelin::protocol::CommandReply
        dispatchRemote(const javelin::protocol::CommandId& id,
                       const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchAccountAction(const javelin::protocol::CommandId& id,
                              const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchCalendarAction(const javelin::protocol::CommandId& id,
                               const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchComposeAction(const javelin::protocol::CommandId& id,
                              const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchContactAction(const javelin::protocol::CommandId& id,
                              const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchMailAction(const javelin::protocol::CommandId& id,
                           const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchSieveAction(const javelin::protocol::CommandId& id,
                            const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchIdentityAction(const javelin::protocol::CommandId& id,
                               const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchHistoryAction(const javelin::protocol::CommandId& id,
                              const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchWorkAction(const javelin::protocol::CommandId& id,
                           const javelin::protocol::RemoteActionCommand& command);
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchDeveloperAction(const javelin::protocol::CommandId& id,
                                const javelin::protocol::RemoteActionCommand& command);

        template <typename Action, typename Apply>
        [[nodiscard]] javelin::protocol::CommandReply
        dispatchDecoded(const javelin::protocol::CommandId& id,
                        const javelin::protocol::RemoteActionCommand& command, Apply&& apply)
        {
            auto decoded = remote::decodeVersionedTuple<Action::requestSchemaVersion,
                                                        typename Action::Request>(command.payload);
            if (const auto* error = std::get_if<remote::CodecError>(&decoded))
                return reject(id, error->message);
            return std::apply(std::forward<Apply>(apply),
                              std::get<typename Action::Request>(std::move(decoded)));
        }

        template <typename Action>
        [[nodiscard]] javelin::protocol::CommandReply
        acceptValue(const javelin::protocol::CommandId& id,
                    const typename Action::Result& value) const
        {
            static_assert(Action::admission ==
                          javelin::protocol::actions::AdmissionSemantics::Immediate);
            auto encoded = remote::encodeVersioned<Action::resultSchemaVersion>(value);
            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                return reject(id, error->message);
            return acceptImmediate(id, Action::id, std::get<QByteArray>(std::move(encoded)));
        }

        template <typename Action>
        [[nodiscard]] javelin::protocol::CommandReply
        acceptEmpty(const javelin::protocol::CommandId& id) const
        {
            static_assert(std::same_as<typename Action::Result, std::monostate>);
            return acceptValue<Action>(id, std::monostate{});
        }

        template <typename Action>
        [[nodiscard]] javelin::protocol::CommandReply
        launchAction(const javelin::protocol::CommandId& id,
                     QCoro::Task<typename Action::Result> task)
        {
            static_assert(Action::admission ==
                          javelin::protocol::actions::AdmissionSemantics::Asynchronous);
            const javelin::protocol::OperationId operation{.value = id.value};
            QTimer::singleShot(
                0, this,
                [this, operation, task = std::move(task)]() mutable
                {
                    QCoro::connect(
                        std::move(task), this,
                        [this, operation](typename Action::Result result)
                        {
                            auto encoded =
                                remote::encodeVersioned<Action::resultSchemaVersion>(result);
                            if (const auto* error = std::get_if<remote::CodecError>(&encoded))
                                fail(operation, error->message);
                            else
                                complete(operation, std::get<QByteArray>(std::move(encoded)));
                        });
                });
            return acceptAsync(id, Action::id, operation);
        }
        [[nodiscard]] javelin::protocol::CommandReply
        reject(const javelin::protocol::CommandId& id, QString detail,
               javelin::protocol::BoundaryErrorCode code =
                   javelin::protocol::BoundaryErrorCode::InvalidRequest) const;
        [[nodiscard]] javelin::protocol::CommandReply
        acceptImmediate(const javelin::protocol::CommandId& id, javelin::protocol::ActionId action,
                        QByteArray result) const;
        [[nodiscard]] javelin::protocol::CommandReply
        acceptAsync(const javelin::protocol::CommandId& id, javelin::protocol::ActionId action,
                    const javelin::protocol::OperationId& operation) const;
        [[nodiscard]] QCoro::Task<AccountAuthenticationResult>
        finishOAuthAndFilter(OAuthFinishRequest request);
        [[nodiscard]] QCoro::Task<AccountAuthenticationResult>
        authenticateManuallyAndFilter(ManualAuthenticationRequest request);
        [[nodiscard]] QCoro::Task<RemoteUndoExecutionResult> performUndo(bool redo);
        void complete(const javelin::protocol::OperationId& operation, QByteArray result);
        void fail(const javelin::protocol::OperationId& operation, QString detail);
        void setReplayReply(ReplayEntry& entry,
                            std::optional<javelin::protocol::CommandReply> reply);
        void eraseReplay(const QString& key);
        void expireReplayResult(ReplayEntry& entry);
        void trimReplays();

        [[nodiscard]] static QString replayKey(const javelin::protocol::CommandId& id);

        DaemonServices& m_services;
        javelin::protocol::BoundaryEventSink& m_eventSink;
        std::function<javelin::protocol::InvalidationEpoch()> m_currentEpoch;
        std::function<std::optional<javelin::protocol::BoundaryError>()> m_reloadSettings;
        AuthenticationResultFilter m_authenticationResultFilter;
        ConnectionSettingsHydrator m_connectionSettingsHydrator;
        RevocationRequestHydrator m_revocationRequestHydrator;
        std::unordered_map<QString, ReplayEntry> m_replays;
        std::deque<QString> m_replayOrder;
        std::size_t m_replayResultBytes = 0;
        std::unordered_map<QString, std::unique_ptr<MailboxObservation>> m_mailboxObservations;
        bool m_daemonLogSubscribed = false;
    };
} // namespace javelin::app
