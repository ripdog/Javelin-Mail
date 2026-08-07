#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/OnboardingTypes.h"
#include "app/RemoteActionTypes.h"
#include "protocol/ProcessBoundary.h"

#include <QCoroTask>

#include <QObject>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
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
            javelin::protocol::RemoteActionKind kind =
                javelin::protocol::RemoteActionKind::RemoveConfiguredAccount;
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
        reject(const javelin::protocol::CommandId& id, QString detail,
               javelin::protocol::BoundaryErrorCode code =
                   javelin::protocol::BoundaryErrorCode::InvalidRequest) const;
        [[nodiscard]] javelin::protocol::CommandReply
        acceptImmediate(const javelin::protocol::CommandId& id,
                        javelin::protocol::RemoteActionKind kind, QByteArray result) const;
        [[nodiscard]] javelin::protocol::CommandReply
        acceptAsync(const javelin::protocol::CommandId& id,
                    javelin::protocol::RemoteActionKind kind,
                    const javelin::protocol::OperationId& operation) const;
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
