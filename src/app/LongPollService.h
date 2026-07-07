#pragma once

#include "jmap/JmapCore.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/QueryService.h"
#include "jmap/sync/EventSourceLongPoll.h"
#include "jmap/sync/LongPollWorker.h"
#include "jmap/sync/RefreshNotificationTypes.h"

#include <QCoroTask>

#include <QObject>
#include <QTimer>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class QNetworkAccessManager;

namespace javelin::app
{

    class LongPollService final : public QObject,
                                  public javelin::jmap::sync::AbstractLongPollObserver
    {
        Q_OBJECT

      public:
        enum class Status
        {
            Disconnected,
            Connecting,
            Connected,
        };
        Q_ENUM(Status)

        LongPollService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                        javelin::jmap::api::AbstractTransport& transport,
                        QNetworkAccessManager& networkAccessManager,
                        javelin::jmap::cache::AccountRepository& accountRepository,
                        javelin::jmap::cache::QueryService& queryService,
                        QObject* parent = nullptr);
        ~LongPollService() override;

        void applySettings(javelin::jmap::LiveConnectionSettings settings);
        void stop();

        [[nodiscard]] Status status() const;
        [[nodiscard]] QCoro::Task<void>
        onUpdate(javelin::jmap::sync::LongPollResponse response) override;

      Q_SIGNALS:
        void statusChanged(javelin::app::LongPollService::Status status);
        void mailStateChanged(const QString& accountId, bool requiresCatchUpRefresh);
        void accountMailStateChanged(const QString& accountId, const QString& refreshedMailboxId);
        void mailboxRefreshed(const QString& accountId, const QString& mailboxId,
                              bool scrollToNewest);
        void notificationRaised(const QString& accountId, const QString& mailboxId,
                                const QString& threadId, const QString& emailId,
                                const QString& mailboxName, const QString& title,
                                const QString& message);

      private:
        struct RunConfiguration
        {
            javelin::jmap::LiveConnectionSettings settings;
            std::string accountId;
            std::string mailboxId;
            std::string mailboxName;
            std::string apiUrl;
            std::string eventSourceUrl;
        };

        struct RunContext
        {
            std::size_t generation = 0;
            RunConfiguration configuration;
            javelin::jmap::sync::LongPollCancellation cancellation;
            javelin::jmap::sync::QtLongPollSleeper sleeper;
            std::unique_ptr<javelin::jmap::sync::EventSourceLongPollChannel> channel;
            std::unique_ptr<javelin::jmap::sync::LongPollWorker> worker;
        };

        [[nodiscard]] bool hasValidSettings() const;
        [[nodiscard]] std::optional<RunConfiguration> resolveConfiguration() const;
        [[nodiscard]] QCoro::Task<void> runLoop(std::shared_ptr<RunContext> runContext);
        [[nodiscard]] QCoro::Task<void> refreshWatchedMailbox();
        [[nodiscard]] QCoro::Task<void>
        refreshWatchedMailboxOnce(std::shared_ptr<RunContext> runContext);
        [[nodiscard]] QCoro::Task<bool>
        refreshMailboxStateOnce(std::shared_ptr<RunContext> runContext);
        void handleResumeWatchdogTimeout();
        void scheduleDebouncedRefresh();
        void scheduleCatchUpRefresh();
        void restartForCatchUp();
        void restart();
        void setStatus(Status status);
        void publishNotifications(
            const RunContext& runContext, std::string_view mailboxName,
            const std::vector<javelin::jmap::sync::RefreshNotificationCandidate>& candidates);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::AbstractTransport& m_transport;
        QNetworkAccessManager& m_networkAccessManager;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        std::optional<javelin::jmap::LiveConnectionSettings> m_settings;
        std::shared_ptr<RunContext> m_runContext;
        std::string m_lastEventId;
        std::deque<std::string> m_recentNotificationKeys;
        std::unordered_set<std::string> m_notifiedEmailKeys;
        std::size_t m_generation = 0;
        Status m_status = Status::Disconnected;
        bool m_shouldCatchUpRefreshOnReconnect = false;
        bool m_refreshInFlight = false;
        bool m_refreshAgainRequested = false;
        QTimer m_refreshDebounceTimer;
        QTimer m_resumeWatchdogTimer;
        qint64 m_lastResumeWatchdogTickMs = 0;
    };

} // namespace javelin::app
