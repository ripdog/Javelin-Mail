#pragma once

#include "app/AccountConnectionSettings.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Session.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/QueryService.h"
#include "jmap/sync/EventSourceLongPoll.h"
#include "jmap/sync/LongPollWorker.h"
#include "jmap/sync/RefreshNotificationTypes.h"
#include "jmap/sync/WebSocketPushChannel.h"

#include <QCoroTask>

#include <QObject>
#include <QStringList>
#include <QTimer>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QNetworkAccessManager;

namespace javelin::app
{
    class WorkScheduler;
    struct MailboxQueryWindowChange
    {
        QString mailboxId;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::optional<std::size_t> total;
    };

    struct SearchQueryWindowChange
    {
        QString queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::optional<std::size_t> total;
    };

    struct MailCacheChange
    {
        QString accountId;
        QStringList mailboxIds;
        std::vector<MailboxQueryWindowChange> queryWindows;
        std::vector<SearchQueryWindowChange> searchWindows;
        bool mailboxTreeChanged = false;
        bool hasNewMail = false;
    };

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

        AccountSyncCoordinator(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                               javelin::jmap::api::JmapMethodTransport& methodTransport,
                               QNetworkAccessManager& networkAccessManager,
                               javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
                               javelin::jmap::cache::AccountRepository& accountRepository,
                               javelin::jmap::cache::QueryService& queryService,
                               WorkScheduler& workScheduler, QObject* parent = nullptr);
        ~AccountSyncCoordinator() override;

        void applySettings(AccountConnectionSettings settings, std::string accountId,
                           std::vector<std::string> mailboxIds,
                           std::vector<std::string> notificationMailboxIds,
                           bool notificationMailboxSelectionConfigured);
        void stop();
        void pauseForAuthentication();
        [[nodiscard]] bool requestSynchronization();

        [[nodiscard]] Status status() const;
        [[nodiscard]] QCoro::Task<void>
        onStateChange(javelin::jmap::sync::StateChangeEvent event) override;

      Q_SIGNALS:
        void statusChanged(javelin::app::AccountSyncCoordinator::Status status);
        void cacheCommitted(javelin::app::MailCacheChange change);
        void calendarStateChanged(const QString& ownerAccountId);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message);
        void operationFailed(const QString& operation, javelin::jmap::OperationError error);

      private:
        struct RunConfiguration
        {
            AccountConnectionSettings settings;
            std::string accountId;
            std::vector<std::pair<std::string, std::string>> mailboxes;
            std::unordered_set<std::string> notificationMailboxIds;
            std::string apiUrl;
            std::string eventSourceUrl;
            std::optional<javelin::jmap::api::WebSocketCapability> websocket;
            bool calendarCapable = false;
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

        [[nodiscard]] bool hasValidSettings() const;
        [[nodiscard]] std::optional<RunConfiguration> resolveConfiguration() const;
        [[nodiscard]] QCoro::Task<void> runLoop(std::shared_ptr<RunContext> runContext);
        [[nodiscard]] QCoro::Task<void> refreshWatchedMailbox(bool refreshEmailMailboxes);
        [[nodiscard]] QCoro::Task<void>
        refreshWatchedMailboxOnce(std::shared_ptr<RunContext> runContext,
                                  bool refreshEmailMailboxes);
        [[nodiscard]] QCoro::Task<bool>
        refreshMailboxStateOnce(std::shared_ptr<RunContext> runContext);
        void handleResumeWatchdogTimeout();
        void scheduleDebouncedRefresh(bool forceEmailRefresh = false);
        void scheduleCatchUpRefresh();
        [[nodiscard]] bool pendingStateChangeAlreadyApplied(std::string_view type,
                                                            std::string_view state) const;
        [[nodiscard]] bool pendingStateChangesAlreadyApplied() const;
        [[nodiscard]] bool watchedMailboxCoverageIsAuthoritative() const;
        void restartForCatchUp();
        void restart();
        void setStatus(Status status);
        void handleOperationError(const QString& operation,
                                  const javelin::jmap::OperationError& error);
        void publishNotifications(
            const RunContext& runContext, std::string_view mailboxId, std::string_view mailboxName,
            const std::vector<javelin::jmap::sync::RefreshNotificationCandidate>& candidates);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::JmapMethodTransport& m_methodTransport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::api::WebSocketFailureCooldowns& m_transportCooldowns;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        WorkScheduler& m_workScheduler;
        std::optional<AccountConnectionSettings> m_settings;
        std::string m_accountId;
        std::vector<std::string> m_mailboxIds;
        std::shared_ptr<RunContext> m_runContext;
        std::string m_lastEventId;
        std::vector<std::string> m_notificationMailboxIds;
        bool m_notificationMailboxSelectionConfigured = false;
        std::unordered_map<std::string, std::string> m_pendingStateChanges;
        std::size_t m_generation = 0;
        Status m_status = Status::Disconnected;
        bool m_shouldCatchUpRefreshOnReconnect = false;
        bool m_refreshInFlight = false;
        bool m_refreshAgainRequested = false;
        bool m_refreshEmailAgainRequested = false;
        bool m_forceEmailRefreshRequested = false;
        QTimer m_refreshDebounceTimer;
        QTimer m_resumeWatchdogTimer;
        qint64 m_lastResumeWatchdogTickMs = 0;
    };

} // namespace javelin::app
