#pragma once

#include "app/AccountConnectionSettings.h"
#include "app/MailApplicationTypes.h"
#include "app/account/EndpointRetryGate.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/sync/EventSourceLongPoll.h"
#include "jmap/sync/LongPollWorker.h"
#include "jmap/sync/RefreshNotificationTypes.h"
#include "jmap/sync/WebSocketPushChannel.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QNetworkAccessManager;

namespace javelin::app
{
    class WorkScheduler;

    class AccountSyncCoordinator final : public QObject,
                                         public javelin::jmap::sync::StateChangeConsumer
    {
        Q_OBJECT

      public:
        enum class Status
        {
            Disconnected,
            Connecting,
            Connected,
            AuthenticationPaused,
        };
        Q_ENUM(Status)

        AccountSyncCoordinator(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::api::JmapMethodTransport& methodTransport,
            QNetworkAccessManager& networkAccessManager,
            javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
            javelin::jmap::cache::AccountRepository& accountRepository,
            javelin::jmap::cache::MailboxReader& mailboxReader, WorkScheduler& workScheduler,
            EndpointRetryGate& endpointRetryGate,
            javelin::jmap::auth::AccessTokenRefreshHandler authenticationRefreshHandler = {},
            QObject* parent = nullptr);
        ~AccountSyncCoordinator() override;

        void applySettings(AccountConnectionSettings settings, std::string accountId,
                           std::vector<std::string> mailboxIds,
                           std::vector<std::string> notificationMailboxIds);
        void stop();
        void pauseForAuthentication();
        void networkBecameReachable();
        [[nodiscard]] bool requestSynchronization();
        [[nodiscard]] bool requestMailboxSynchronization(std::string_view mailboxId);
        [[nodiscard]] Status status() const;

        [[nodiscard]] QCoro::Task<void>
        onStateChange(javelin::jmap::sync::StateChangeEvent event) override;

      Q_SIGNALS:
        void statusChanged(javelin::app::AccountSyncCoordinator::Status status);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void calendarStateChanged(const QString& ownerAccountId,
                                  const javelin::jmap::sync::AccountTypeStateMap& changedStates);
        void contactStateChanged(const QString& ownerAccountId,
                                 const javelin::jmap::sync::AccountTypeStateMap& changedStates);
        void identityStateChanged(const QString& ownerAccountId,
                                  const javelin::jmap::sync::AccountTypeStateMap& changedStates);
        void notificationMailboxRefreshed(const QString& accountId, const QString& mailboxId,
                                          const QString& mailboxName);
        void operationFailed(const QString& operation, javelin::jmap::OperationError error);
        void operationSucceeded();
        void stateChangeCatchUpRequired();

      private:
        struct RunConfiguration
        {
            AccountConnectionSettings settings;
            std::string accountId;
            std::string remoteAccountId;
            std::vector<std::pair<std::string, std::string>> mailboxes;
            std::unordered_set<std::string> notificationMailboxIds;
            std::string apiUrl;
            javelin::jmap::api::CoreRequestLimits requestLimits;
            std::string eventSourceUrl;
            std::optional<javelin::jmap::api::WebSocketCapability> websocket;
            std::vector<std::string> groupwareAccountIds;
            bool calendarCapable = false;
            bool contactsCapable = false;
            bool identitiesCapable = false;
        };

        struct RunContext
        {
            std::size_t generation = 0;
            RunConfiguration configuration;
            javelin::jmap::sync::StateChangeCancellation cancellation;
            javelin::jmap::sync::QtStateChangeSleeper sleeper;
            std::unique_ptr<javelin::jmap::sync::StateChangeSource> source;
            std::unique_ptr<javelin::jmap::sync::StateChangeWorker> worker;
        };

        struct MailRefreshDemand
        {
            bool mailboxState = false;
            bool emailState = false;
            bool allMailboxes = false;
            std::vector<std::string> mailboxIds;

            [[nodiscard]] bool empty() const
            {
                return !mailboxState && !emailState && !allMailboxes && mailboxIds.empty();
            }

            void merge(const MailRefreshDemand& other)
            {
                mailboxState = mailboxState || other.mailboxState;
                emailState = emailState || other.emailState;
                if (other.allMailboxes)
                {
                    allMailboxes = true;
                    mailboxIds.clear();
                    return;
                }
                if (allMailboxes)
                    return;
                for (const auto& mailboxId : other.mailboxIds)
                {
                    if (std::ranges::find(mailboxIds, mailboxId) == mailboxIds.end())
                        mailboxIds.push_back(mailboxId);
                }
            }

            [[nodiscard]] static MailRefreshDemand full()
            {
                return {.mailboxState = true,
                        .emailState = true,
                        .allMailboxes = true,
                        .mailboxIds = {}};
            }
        };

        [[nodiscard]] bool hasValidSettings() const;
        [[nodiscard]] std::optional<RunConfiguration> resolveConfiguration() const;
        [[nodiscard]] QCoro::Task<void> runLoop(std::shared_ptr<RunContext> runContext);
        [[nodiscard]] QCoro::Task<void> refreshWatchedMailbox(MailRefreshDemand demand,
                                                              EndpointRetryGate::Lease retryLease);
        [[nodiscard]] QCoro::Task<void>
        refreshWatchedMailboxOnce(std::shared_ptr<RunContext> runContext, MailRefreshDemand demand,
                                  EndpointRetryGate::Lease& retryLease);
        [[nodiscard]] QCoro::Task<std::optional<bool>>
        refreshMailboxStateOnce(std::shared_ptr<RunContext> runContext,
                                EndpointRetryGate::Lease& retryLease);
        void handleResumeWatchdogTimeout();
        void scheduleDebouncedRefresh(bool forceEmailRefresh = false,
                                      std::vector<std::string> mailboxIds = {});
        void scheduleCatchUpRefresh();
        void processGroupwareStateChanges();
        [[nodiscard]] bool domainHasActiveMutation(std::string_view accountId,
                                                   std::string_view type) const;
        [[nodiscard]] bool stateChangeAlreadyApplied(std::string_view accountId,
                                                     std::string_view type,
                                                     std::string_view state) const;
        [[nodiscard]] bool pendingStateChangeAlreadyApplied(std::string_view type,
                                                            std::string_view state) const;
        [[nodiscard]] bool pendingStateChangesAlreadyApplied() const;
        void restartForCatchUp();
        void restart();
        void setStatus(Status status);
        void handleStateChangeAuthenticationError(const QString& operation,
                                                  const javelin::jmap::OperationError& error);
        [[nodiscard]] QCoro::Task<void>
        recoverStateChangeAuthentication(QString operation, javelin::jmap::OperationError error,
                                         std::size_t generation, std::string accountId,
                                         std::string rejectedAccessToken);
        void publishOperationError(const QString& operation,
                                   const javelin::jmap::OperationError& error);
        void recordRefreshFailure(EndpointRetryGate::Lease& retryLease,
                                  const javelin::jmap::OperationError& error);
        void recordRefreshSuccess(EndpointRetryGate::Lease& retryLease);
        [[nodiscard]] static bool usesEndpointBackoff(const javelin::jmap::OperationError& error);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns& m_transportCooldowns;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        WorkScheduler& m_workScheduler;
        EndpointRetryGate& m_endpointRetryGate;
        javelin::jmap::auth::AccessTokenRefreshHandler m_authenticationRefreshHandler;
        std::optional<AccountConnectionSettings> m_settings;
        std::string m_accountId;
        std::vector<std::string> m_mailboxIds;
        std::shared_ptr<RunContext> m_runContext;
        std::string m_lastEventId;
        std::vector<std::string> m_notificationMailboxIds;
        std::unordered_map<std::string, std::string> m_pendingStateChanges;
        javelin::jmap::sync::AccountTypeStateMap m_pendingCalendarStateChanges;
        javelin::jmap::sync::AccountTypeStateMap m_pendingContactStateChanges;
        javelin::jmap::sync::AccountTypeStateMap m_pendingIdentityStateChanges;
        std::size_t m_generation = 0;
        Status m_status = Status::Disconnected;
        bool m_shouldCatchUpRefreshOnReconnect = false;
        bool m_refreshInFlight = false;
        bool m_authenticationRecoveryInFlight = false;
        std::optional<std::string> m_stateChangeAuthenticationRetryToken;
        std::optional<std::size_t> m_refreshGenerationInFlight;
        MailRefreshDemand m_queuedRefreshDemand;
        MailRefreshDemand m_debouncedRefreshDemand;
        QTimer m_refreshDebounceTimer;
        QTimer m_resumeWatchdogTimer;
        qint64 m_lastResumeWatchdogTickMs = 0;
    };

} // namespace javelin::app
