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

        [[nodiscard]] LongPollService::Status
        toServiceStatus(const javelin::jmap::sync::LongPollConnectionStatus status)
        {
            switch (status)
            {
            case javelin::jmap::sync::LongPollConnectionStatus::Disconnected:
                return LongPollService::Status::Disconnected;
            case javelin::jmap::sync::LongPollConnectionStatus::Connecting:
                return LongPollService::Status::Connecting;
            case javelin::jmap::sync::LongPollConnectionStatus::Connected:
                return LongPollService::Status::Connected;
            }

            return LongPollService::Status::Disconnected;
        }

    } // namespace

    LongPollService::LongPollService(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                     javelin::jmap::api::AbstractTransport& transport,
                                     QNetworkAccessManager& networkAccessManager,
                                     javelin::jmap::cache::AccountRepository& accountRepository,
                                     javelin::jmap::cache::QueryService& queryService,
                                     QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_transport(transport),
          m_networkAccessManager(networkAccessManager), m_accountRepository(accountRepository),
          m_queryService(queryService)
    {
        m_refreshDebounceTimer.setSingleShot(true);
        m_refreshDebounceTimer.setInterval(refreshDebounceInterval);
        QObject::connect(&m_refreshDebounceTimer, &QTimer::timeout, this,
                         &LongPollService::scheduleCatchUpRefresh);
        m_lastResumeWatchdogTickMs = QDateTime::currentMSecsSinceEpoch();
        m_resumeWatchdogTimer.setInterval(resumeWatchdogInterval);
        QObject::connect(&m_resumeWatchdogTimer, &QTimer::timeout, this,
                         &LongPollService::handleResumeWatchdogTimeout);
        m_resumeWatchdogTimer.start();
    }

    LongPollService::~LongPollService()
    {
        stop();
    }

    void LongPollService::applySettings(javelin::jmap::LiveConnectionSettings settings)
    {
        if (m_settings.has_value() && m_runContext != nullptr &&
            m_settings->sessionUrl == settings.sessionUrl &&
            m_settings->loginEmail == settings.loginEmail && m_settings->apiKey == settings.apiKey)
        {
            return;
        }

        m_settings = std::move(settings);
        restart();
    }

    void LongPollService::stop()
    {
        if (m_runContext != nullptr)
        {
            if (m_runContext->channel != nullptr)
            {
                m_runContext->channel->cancel();
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

    LongPollService::Status LongPollService::status() const
    {
        return m_status;
    }

    QCoro::Task<void> LongPollService::onUpdate(javelin::jmap::sync::LongPollResponse response)
    {
        m_lastEventId = response.newState;
        scheduleDebouncedRefresh();
        co_return;
    }

    bool LongPollService::hasValidSettings() const
    {
        return m_settings.has_value() && !m_settings->sessionUrl.empty() &&
               !m_settings->loginEmail.empty() && !m_settings->apiKey.empty();
    }

    std::optional<LongPollService::RunConfiguration> LongPollService::resolveConfiguration() const
    {
        if (!hasValidSettings())
        {
            return std::nullopt;
        }

        const auto accountsResult = m_accountRepository.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr || accounts->empty())
        {
            return std::nullopt;
        }

        const auto primaryIt =
            std::ranges::find_if(*accounts, [](const auto& account) { return account.isPrimary; });
        const auto& account = primaryIt != accounts->end() ? *primaryIt : accounts->front();

        javelin::jmap::cache::SessionRepository sessionRepository{m_databaseConnection};
        const auto sessionResult = sessionRepository.load(account.accountId);
        const auto* session =
            std::get_if<std::optional<javelin::jmap::api::Session>>(&sessionResult);
        if (session == nullptr || !session->has_value() || !(*session)->eventSourceUrl.has_value())
        {
            qWarning() << "Long poll configuration unavailable because the cached session has no "
                          "eventSourceUrl";
            return std::nullopt;
        }

        const auto mailboxTreeResult = m_queryService.listMailboxTree(account.accountId);
        const auto* mailboxTree =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxTreeResult);
        if (mailboxTree == nullptr || mailboxTree->empty())
        {
            qWarning() << "Long poll configuration unavailable because the mailbox tree is empty";
            return std::nullopt;
        }

        const auto inboxIt = std::ranges::find_if(
            *mailboxTree, [](const auto& mailbox)
            { return mailbox.role == std::optional<std::string>{std::string{"inbox"}}; });
        const auto& mailbox = inboxIt != mailboxTree->end() ? *inboxIt : mailboxTree->front();

        return RunConfiguration{
            .settings = *m_settings,
            .accountId = account.accountId,
            .mailboxId = mailbox.id,
            .mailboxName = mailbox.name,
            .apiUrl = session->value().apiUrl,
            .eventSourceUrl = *session->value().eventSourceUrl,
        };
    }

    QCoro::Task<void> LongPollService::runLoop(std::shared_ptr<RunContext> runContext)
    {
        javelin::jmap::sync::LongPollRequest request{
            .accountId = runContext->configuration.accountId,
            .eventSourceUrl = runContext->configuration.eventSourceUrl,
            .lastState = m_lastEventId,
            .types = {"Email", "Mailbox"},
        };

        co_await runContext->worker->run(request, runContext->cancellation);
    }

    QCoro::Task<void> LongPollService::refreshWatchedMailbox()
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
    LongPollService::refreshWatchedMailboxOnce(std::shared_ptr<RunContext> runContext)
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

        javelin::jmap::api::MethodCaller methodCaller{m_transport};
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
        const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
            runContext->configuration.accountId, runContext->configuration.mailboxId, {});
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        bool watchedMailboxRefreshed = false;
        if (const auto* summary =
                std::get_if<javelin::jmap::sync::MailboxRefreshSummary>(&refreshResult))
        {
            watchedMailboxRefreshed = true;
            Q_EMIT mailboxRefreshed(QString::fromStdString(runContext->configuration.accountId),
                                    QString::fromStdString(runContext->configuration.mailboxId),
                                    !summary->insertedEmailIds.empty());
            publishNotifications(*runContext, runContext->configuration.mailboxName,
                                 summary->notificationCandidates);
        }
        else if (const auto* error =
                     std::get_if<javelin::jmap::sync::MailboxRefreshError>(&refreshResult))
        {
            qWarning().noquote() << "Long poll mailbox refresh failed" << error->message;
        }

        if (mailboxStateRefreshed || watchedMailboxRefreshed)
        {
            Q_EMIT accountMailStateChanged(
                QString::fromStdString(runContext->configuration.accountId),
                watchedMailboxRefreshed
                    ? QString::fromStdString(runContext->configuration.mailboxId)
                    : QString{});
        }
    }

    QCoro::Task<bool>
    LongPollService::refreshMailboxStateOnce(std::shared_ptr<RunContext> runContext)
    {
        if (runContext == nullptr || !hasValidSettings() ||
            runContext->cancellation.isCancelled() || m_runContext == nullptr ||
            m_runContext->generation != runContext->generation)
        {
            co_return false;
        }

        javelin::jmap::api::MethodCaller methodCaller{m_transport};
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
            qWarning().noquote() << "Long poll mailbox state refresh failed" << error->message;
            co_return false;
        }

        co_return true;
    }

    void LongPollService::handleResumeWatchdogTimeout()
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

        qWarning().noquote() << "Long poll resume watchdog detected event-loop stall" << elapsedMs
                             << "ms; restarting event-source stream";
        restartForCatchUp();
    }

    void LongPollService::scheduleDebouncedRefresh()
    {
        if (!hasValidSettings() || m_runContext == nullptr)
        {
            return;
        }

        m_refreshDebounceTimer.start();
    }

    void LongPollService::scheduleCatchUpRefresh()
    {
        auto task = refreshWatchedMailbox();
        QCoro::connect(std::move(task), this, []() {});
    }

    void LongPollService::restartForCatchUp()
    {
        if (!hasValidSettings())
        {
            return;
        }

        restart();
        if (m_runContext != nullptr)
        {
            m_shouldCatchUpRefreshOnReconnect = true;
        }
    }

    void LongPollService::restart()
    {
        const auto nextConfiguration = resolveConfiguration();
        if (!nextConfiguration.has_value())
        {
            qWarning() << "Long poll restart aborted because no configuration could be resolved";
            stop();
            return;
        }

        const bool sameStream =
            m_runContext != nullptr &&
            m_runContext->configuration.accountId == nextConfiguration->accountId &&
            m_runContext->configuration.mailboxId == nextConfiguration->mailboxId &&
            m_runContext->configuration.eventSourceUrl == nextConfiguration->eventSourceUrl;
        if (!sameStream)
        {
            m_lastEventId.clear();
        }

        stop();

        auto runContext = std::make_shared<RunContext>();
        runContext->generation = ++m_generation;
        runContext->configuration = *nextConfiguration;
        runContext->channel = std::make_unique<javelin::jmap::sync::EventSourceLongPollChannel>(
            m_networkAccessManager, nextConfiguration->settings.apiKey,
            [this, generation = runContext->generation](
                const javelin::jmap::sync::LongPollConnectionStatus status)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                {
                    return;
                }

                setStatus(toServiceStatus(status));
            });
        runContext->worker = std::make_unique<javelin::jmap::sync::LongPollWorker>(
            *runContext->channel, *this, runContext->sleeper, javelin::jmap::sync::BackoffPolicy{},
            [this, generation = runContext->generation](
                const javelin::jmap::sync::LongPollConnectionStatus status)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                {
                    return;
                }

                if (status == javelin::jmap::sync::LongPollConnectionStatus::Connected)
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

    void LongPollService::setStatus(const Status status)
    {
        if (m_status == status)
        {
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
            m_shouldCatchUpRefreshOnReconnect = false;
            qInfo().noquote() << "Long poll scheduling reconnect catch-up refresh"
                              << QString::fromStdString(m_runContext->configuration.accountId)
                              << QString::fromStdString(m_runContext->configuration.mailboxId);
            scheduleCatchUpRefresh();
        }
        Q_EMIT statusChanged(m_status);
    }

    void LongPollService::publishNotifications(
        const RunContext& runContext, const std::string_view mailboxName,
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
            const auto key = runContext.configuration.accountId + '\n' +
                             runContext.configuration.mailboxId + '\n' + candidate.emailId;
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
                                  QString::fromStdString(runContext.configuration.mailboxId),
                                  QString::fromStdString(target.threadId),
                                  QString::fromStdString(target.emailId),
                                  QString::fromStdString(std::string{mailboxName}), title, message);
    }

} // namespace javelin::app
