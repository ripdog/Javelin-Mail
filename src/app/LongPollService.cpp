#include "app/LongPollService.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/Session.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/MailboxStateRefreshExecutor.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QNetworkAccessManager>

#include <chrono>
#include <ranges>

namespace javelin::app
{

    namespace
    {
        constexpr std::size_t maxRecentNotificationKeys = 512;
        constexpr auto refreshDebounceInterval = std::chrono::milliseconds{750};
        constexpr auto resumeWatchdogInterval = std::chrono::seconds{30};
        constexpr auto resumeWatchdogStallThreshold = std::chrono::seconds{90};

        [[nodiscard]] AccountSyncCoordinator::Status
        toServiceStatus(const javelin::jmap::sync::StateChangeConnectionStatus status)
        {
            switch (status)
            {
            case javelin::jmap::sync::StateChangeConnectionStatus::Disconnected:
                return AccountSyncCoordinator::Status::Disconnected;
            case javelin::jmap::sync::StateChangeConnectionStatus::Connecting:
                return AccountSyncCoordinator::Status::Connecting;
            case javelin::jmap::sync::StateChangeConnectionStatus::Connected:
                return AccountSyncCoordinator::Status::Connected;
            }

            return AccountSyncCoordinator::Status::Disconnected;
        }

    } // namespace

    AccountSyncCoordinator::AccountSyncCoordinator(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                     javelin::jmap::api::JmapMethodTransport& methodTransport,
                                     QNetworkAccessManager& networkAccessManager,
                                     javelin::jmap::cache::AccountRepository& accountRepository,
                                     javelin::jmap::cache::QueryService& queryService,
                                     QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_methodTransport(methodTransport), m_networkAccessManager(networkAccessManager),
          m_accountRepository(accountRepository), m_queryService(queryService)
    {
        m_refreshDebounceTimer.setSingleShot(true);
        m_refreshDebounceTimer.setInterval(refreshDebounceInterval);
        QObject::connect(&m_refreshDebounceTimer, &QTimer::timeout, this,
                         &AccountSyncCoordinator::scheduleCatchUpRefresh);
        m_lastResumeWatchdogTickMs = QDateTime::currentMSecsSinceEpoch();
        m_resumeWatchdogTimer.setInterval(resumeWatchdogInterval);
        QObject::connect(&m_resumeWatchdogTimer, &QTimer::timeout, this,
                         &AccountSyncCoordinator::handleResumeWatchdogTimeout);
        m_resumeWatchdogTimer.start();
    }

    AccountSyncCoordinator::~AccountSyncCoordinator()
    {
        stop();
    }

    void AccountSyncCoordinator::applySettings(AccountConnectionSettings settings,
                                               std::string accountId,
                                        std::vector<std::string> mailboxIds)
    {
        if (m_settings.has_value() && m_runContext != nullptr &&
            m_settings->sessionUrl == settings.sessionUrl &&
            m_settings->loginEmail == settings.loginEmail &&
            m_settings->apiKey == settings.apiKey && m_accountId == accountId &&
            m_mailboxIds == mailboxIds)
        {
            return;
        }

        m_settings = std::move(settings);
        m_accountId = std::move(accountId);
        m_mailboxIds = std::move(mailboxIds);
        restart();
    }

    void AccountSyncCoordinator::stop()
    {
        if (m_runContext != nullptr)
        {
            if (m_runContext->source != nullptr)
            {
                m_runContext->source->cancel();
            }
            m_runContext->cancellation.cancel();
            m_runContext.reset();
        }

        m_recentNotificationKeys.clear();
        m_notifiedEmailKeys.clear();
        m_refreshDebounceTimer.stop();
        m_refreshInFlight = false;
        m_refreshAgainRequested = false;
        setStatus(Status::Disconnected);
        m_shouldCatchUpRefreshOnReconnect = false;
    }

    AccountSyncCoordinator::Status AccountSyncCoordinator::status() const
    {
        return m_status;
    }

    bool AccountSyncCoordinator::requestSynchronization()
    {
        if (m_runContext == nullptr)
            return false;
        scheduleDebouncedRefresh();
        return true;
    }

    QCoro::Task<void>
    AccountSyncCoordinator::onStateChange(javelin::jmap::sync::StateChangeEvent event)
    {
        m_lastEventId = event.newState;
        scheduleDebouncedRefresh();
        co_return;
    }

    bool AccountSyncCoordinator::hasValidSettings() const
    {
        return m_settings.has_value() && !m_settings->loginEmail.empty() &&
               !m_settings->apiKey.empty();
    }

    std::optional<AccountSyncCoordinator::RunConfiguration>
    AccountSyncCoordinator::resolveConfiguration() const
    {
        if (!hasValidSettings())
        {
            return std::nullopt;
        }

        javelin::jmap::cache::SessionRepository sessionRepository{m_databaseConnection};
        const auto sessionResult = sessionRepository.load(m_accountId);
        const auto* session =
            std::get_if<std::optional<javelin::jmap::api::Session>>(&sessionResult);
        if (session == nullptr || !session->has_value() ||
            (!(*session)->eventSourceUrl.has_value() &&
             (!(*session)->capabilities.websocket.has_value() ||
              !(*session)->capabilities.websocket->supportsPush)))
        {
            qWarning() << "Account sync configuration unavailable because the cached session has "
                          "no state-change source";
            return std::nullopt;
        }

        const auto mailboxTreeResult = m_queryService.listMailboxTree(m_accountId);
        const auto* mailboxTree =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxTreeResult);
        if (mailboxTree == nullptr || mailboxTree->empty())
        {
            qWarning() << "Account sync configuration unavailable because the mailbox tree is "
                          "empty";
            return std::nullopt;
        }

        std::vector<std::pair<std::string, std::string>> mailboxes;
        for (const auto& mailbox : *mailboxTree)
        {
            if (std::ranges::find(m_mailboxIds, mailbox.id) != m_mailboxIds.end())
            {
                mailboxes.emplace_back(mailbox.id, mailbox.name);
            }
        }

        return RunConfiguration{
            .settings = *m_settings,
            .accountId = m_accountId,
            .mailboxes = std::move(mailboxes),
            .apiUrl = session->value().apiUrl,
            .eventSourceUrl = session->value().eventSourceUrl.value_or(std::string{}),
            .websocket = session->value().capabilities.websocket,
        };
    }

    QCoro::Task<void> AccountSyncCoordinator::runLoop(std::shared_ptr<RunContext> runContext)
    {
        javelin::jmap::sync::StateChangeSubscription subscription{
            .accountId = runContext->configuration.accountId,
            .lastState = m_lastEventId,
            .types = {"Email", "Mailbox"},
        };

        co_await runContext->worker->run(subscription, runContext->cancellation);
    }

    QCoro::Task<void> AccountSyncCoordinator::refreshWatchedMailbox()
    {
        auto runContext = m_runContext;
        if (runContext == nullptr || !hasValidSettings())
        {
            co_return;
        }

        if (m_refreshInFlight)
        {
            m_refreshAgainRequested = true;
            co_return;
        }

        m_refreshInFlight = true;
        const auto generation = runContext->generation;
        do
        {
            m_refreshAgainRequested = false;
            co_await refreshWatchedMailboxOnce(runContext);
            runContext = m_runContext;
        } while (m_refreshAgainRequested && runContext != nullptr &&
                 runContext->generation == generation && !runContext->cancellation.isCancelled());

        if (m_runContext == nullptr || m_runContext->generation == generation)
        {
            m_refreshInFlight = false;
        }
    }

    QCoro::Task<void>
    AccountSyncCoordinator::refreshWatchedMailboxOnce(std::shared_ptr<RunContext> runContext)
    {
        if (runContext == nullptr || !hasValidSettings() ||
            runContext->cancellation.isCancelled() || m_runContext == nullptr ||
            m_runContext->generation != runContext->generation)
        {
            co_return;
        }

        const bool mailboxStateRefreshed = co_await refreshMailboxStateOnce(runContext);
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        javelin::jmap::api::MethodCaller methodCaller{m_methodTransport};
        const javelin::jmap::api::ApiRequestContext apiRequestContext{
            .credentials =
                {
                    .accountId = runContext->configuration.accountId,
                    .emailAddress = runContext->configuration.settings.loginEmail,
                    .sessionUrl = runContext->configuration.settings.sessionUrl,
                    .token =
                        {
                            .accessToken = runContext->configuration.settings.apiKey,
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = runContext->configuration.apiUrl,
        };

        javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
            m_databaseConnection, methodCaller, apiRequestContext};
        bool watchedMailboxRefreshed = false;
        QStringList refreshedMailboxIds;
        bool hasNewMail = false;
        for (const auto& [mailboxId, mailboxName] : runContext->configuration.mailboxes)
        {
            const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                runContext->configuration.accountId, mailboxId, {});
            if (const auto* summary =
                    std::get_if<javelin::jmap::sync::MailboxRefreshSummary>(&refreshResult))
            {
                m_shouldCatchUpRefreshOnReconnect = false;
                watchedMailboxRefreshed = true;
                refreshedMailboxIds.push_back(QString::fromStdString(mailboxId));
                hasNewMail = hasNewMail || !summary->insertedEmailIds.empty();
                publishNotifications(*runContext, mailboxId, mailboxName,
                                     summary->notificationCandidates);
            }
            else if (const auto* error =
                         std::get_if<javelin::jmap::sync::MailboxRefreshError>(&refreshResult))
            {
                qWarning().noquote() << "Push mailbox refresh failed" << error->message;
            }
        }
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        if (mailboxStateRefreshed || watchedMailboxRefreshed)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(runContext->configuration.accountId),
                .mailboxIds = std::move(refreshedMailboxIds),
                .queryWindows = {},
                .searchWindows = {},
                .hasNewMail = hasNewMail,
            });
        }
    }

    QCoro::Task<bool>
    AccountSyncCoordinator::refreshMailboxStateOnce(std::shared_ptr<RunContext> runContext)
    {
        if (runContext == nullptr || !hasValidSettings() ||
            runContext->cancellation.isCancelled() || m_runContext == nullptr ||
            m_runContext->generation != runContext->generation)
        {
            co_return false;
        }

        javelin::jmap::api::MethodCaller methodCaller{m_methodTransport};
        const javelin::jmap::api::ApiRequestContext apiRequestContext{
            .credentials =
                {
                    .accountId = runContext->configuration.accountId,
                    .emailAddress = runContext->configuration.settings.loginEmail,
                    .sessionUrl = runContext->configuration.settings.sessionUrl,
                    .token =
                        {
                            .accessToken = runContext->configuration.settings.apiKey,
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = runContext->configuration.apiUrl,
        };

        javelin::jmap::sync::MailboxStateRefreshExecutor executor{m_databaseConnection,
                                                                  methodCaller, apiRequestContext};
        const auto refreshResult = co_await executor.refresh(runContext->configuration.accountId);
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return false;
        }

        if (const auto* error =
                std::get_if<javelin::jmap::sync::MailboxStateRefreshError>(&refreshResult))
        {
            qWarning().noquote() << "Account sync mailbox state refresh failed" << error->message;
            co_return false;
        }

        co_return true;
    }

    void AccountSyncCoordinator::handleResumeWatchdogTimeout()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 elapsedMs = now - m_lastResumeWatchdogTickMs;
        m_lastResumeWatchdogTickMs = now;

        if (elapsedMs <=
            std::chrono::duration_cast<std::chrono::milliseconds>(resumeWatchdogStallThreshold)
                .count())
        {
            return;
        }

        qWarning().noquote() << "State-change resume watchdog detected event-loop stall"
                             << elapsedMs << "ms; restarting source";
        restartForCatchUp();
    }

    void AccountSyncCoordinator::scheduleDebouncedRefresh()
    {
        if (!hasValidSettings() || m_runContext == nullptr)
        {
            return;
        }

        m_refreshDebounceTimer.start();
    }

    void AccountSyncCoordinator::scheduleCatchUpRefresh()
    {
        auto task = refreshWatchedMailbox();
        QCoro::connect(std::move(task), this, []() {});
    }

    void AccountSyncCoordinator::restartForCatchUp()
    {
        if (!hasValidSettings())
        {
            return;
        }

        restart();
        if (m_runContext != nullptr)
        {
            m_shouldCatchUpRefreshOnReconnect = true;
            qInfo().noquote() << "Account sync scheduling resume catch-up refresh"
                              << QString::fromStdString(m_runContext->configuration.accountId);
            scheduleDebouncedRefresh();
        }
    }

    void AccountSyncCoordinator::restart()
    {
        const auto nextConfiguration = resolveConfiguration();
        if (!nextConfiguration.has_value())
        {
            qWarning() << "Account sync restart aborted because no configuration could be resolved";
            stop();
            return;
        }

        const bool sameStream =
            m_runContext != nullptr &&
            m_runContext->configuration.accountId == nextConfiguration->accountId &&
            m_runContext->configuration.mailboxes == nextConfiguration->mailboxes &&
            m_runContext->configuration.eventSourceUrl == nextConfiguration->eventSourceUrl &&
            m_runContext->configuration.websocket.has_value() ==
                nextConfiguration->websocket.has_value() &&
            (!nextConfiguration->websocket.has_value() ||
             m_runContext->configuration.websocket->url == nextConfiguration->websocket->url);
        if (!sameStream)
        {
            m_lastEventId.clear();
        }

        stop();

        auto runContext = std::make_shared<RunContext>();
        runContext->generation = ++m_generation;
        runContext->configuration = *nextConfiguration;
        const auto sourceStatusCallback =
            [this, generation = runContext->generation](
                const javelin::jmap::sync::StateChangeConnectionStatus status)
        {
            if (m_runContext == nullptr || m_runContext->generation != generation)
            {
                return;
            }

            setStatus(toServiceStatus(status));
        };
        if (nextConfiguration->websocket.has_value() && nextConfiguration->websocket->supportsPush)
        {
            runContext->source = std::make_unique<javelin::jmap::sync::WebSocketStateChangeSource>(
                nextConfiguration->websocket->url, nextConfiguration->settings.apiKey,
                sourceStatusCallback);
        }
        else
        {
            runContext->source =
                std::make_unique<javelin::jmap::sync::EventSourceStateChangeSource>(
                    m_networkAccessManager, nextConfiguration->eventSourceUrl,
                    nextConfiguration->settings.apiKey, sourceStatusCallback);
        }
        runContext->worker = std::make_unique<javelin::jmap::sync::StateChangeWorker>(
            *runContext->source, *this, runContext->sleeper, javelin::jmap::sync::BackoffPolicy{},
            [this, generation = runContext->generation](
                const javelin::jmap::sync::StateChangeConnectionStatus status)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                {
                    return;
                }

                if (status == javelin::jmap::sync::StateChangeConnectionStatus::Connected)
                {
                    return;
                }

                setStatus(toServiceStatus(status));
            });

        m_runContext = runContext;
        setStatus(Status::Connecting);
        auto task = runLoop(runContext);
        QCoro::connect(std::move(task), this,
                       [this, generation = runContext->generation]()
                       {
                           if (m_runContext == nullptr || m_runContext->generation != generation)
                           {
                               return;
                           }

                           setStatus(Status::Disconnected);
                       });
    }

    void AccountSyncCoordinator::setStatus(const Status status)
    {
        if (m_status == status)
        {
            if (status == Status::Disconnected && m_shouldCatchUpRefreshOnReconnect)
            {
                scheduleDebouncedRefresh();
            }
            return;
        }

        const auto previousStatus = m_status;
        m_status = status;
        if (status == Status::Disconnected && previousStatus == Status::Connected)
        {
            m_shouldCatchUpRefreshOnReconnect = true;
        }
        if (status == Status::Connected && m_shouldCatchUpRefreshOnReconnect &&
            m_runContext != nullptr)
        {
            qInfo().noquote() << "Account sync scheduling reconnect catch-up refresh"
                              << QString::fromStdString(m_runContext->configuration.accountId);
            scheduleDebouncedRefresh();
        }
        Q_EMIT statusChanged(m_status);
    }

    void AccountSyncCoordinator::publishNotifications(
        const RunContext& runContext, const std::string_view mailboxId,
        const std::string_view mailboxName,
        const std::vector<javelin::jmap::sync::RefreshNotificationCandidate>& candidates)
    {
        if (candidates.empty())
        {
            return;
        }

        std::vector<javelin::jmap::sync::RefreshNotificationCandidate> newCandidates;
        newCandidates.reserve(candidates.size());
        for (const auto& candidate : candidates)
        {
            const auto key = runContext.configuration.accountId + '\n' + std::string{mailboxId} +
                             '\n' + candidate.emailId;
            if (m_notifiedEmailKeys.contains(key))
            {
                continue;
            }

            m_notifiedEmailKeys.insert(key);
            m_recentNotificationKeys.push_back(key);
            if (m_recentNotificationKeys.size() > maxRecentNotificationKeys)
            {
                m_notifiedEmailKeys.erase(m_recentNotificationKeys.front());
                m_recentNotificationKeys.pop_front();
            }
            newCandidates.push_back(candidate);
        }

        if (newCandidates.empty())
        {
            return;
        }

        const auto& target = newCandidates.front();

        QString title;
        QString message;
        if (newCandidates.size() == 1)
        {
            title = QStringLiteral("New mail in %1")
                        .arg(QString::fromStdString(std::string{mailboxName}));
            message = QString::fromStdString(
                newCandidates.front().subject.value_or(std::string{"(no subject)"}));
        }
        else
        {
            title = QStringLiteral("%1 new messages in %2")
                        .arg(newCandidates.size())
                        .arg(QString::fromStdString(std::string{mailboxName}));
            message = QString::fromStdString(
                newCandidates.front().subject.value_or(std::string{"(no subject)"}));
        }

        Q_EMIT notificationRaised(QString::fromStdString(runContext.configuration.accountId),
                                  QString::fromStdString(std::string{mailboxId}),
                                  QString::fromStdString(target.threadId),
                                  QString::fromStdString(target.emailId),
                                  QString::fromStdString(std::string{mailboxName}), title, message);
    }

} // namespace javelin::app
