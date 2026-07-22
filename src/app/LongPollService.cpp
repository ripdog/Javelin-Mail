#include "app/LongPollService.h"

#include "app/MailboxSyncCoverage.h"
#include "app/WorkScheduler.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/Session.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/MailboxStateRefreshExecutor.h"
#include "jmap/sync/PreferredStateChangeSource.h"

#include <QCoroTimer>
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
        constexpr auto refreshDebounceInterval = std::chrono::milliseconds{750};
        constexpr auto resumeWatchdogInterval = std::chrono::seconds{30};
        constexpr auto resumeWatchdogStallThreshold = std::chrono::seconds{90};

        class ForegroundWorkScope final
        {
          public:
            explicit ForegroundWorkScope(WorkScheduler& scheduler) : m_scheduler(scheduler)
            {
                m_scheduler.beginForegroundWork();
            }
            ~ForegroundWorkScope()
            {
                m_scheduler.endForegroundWork();
            }

          private:
            WorkScheduler& m_scheduler;
        };

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
        javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::QueryService& queryService, WorkScheduler& workScheduler,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_methodTransport(methodTransport), m_networkAccessManager(networkAccessManager),
          m_transportCooldowns(cooldowns), m_accountRepository(accountRepository),
          m_queryService(queryService), m_workScheduler(workScheduler)
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
                                               std::vector<std::string> mailboxIds,
                                               std::vector<std::string> notificationMailboxIds,
                                               const bool notificationMailboxSelectionConfigured)
    {
        const bool resumingAfterAuthentication = m_status == Status::AuthenticationPaused;
        const bool connectionSettingsUnchanged =
            m_settings.has_value() && m_settings->sessionUrl == settings.sessionUrl &&
            m_settings->loginEmail == settings.loginEmail &&
            m_settings->apiKey == settings.apiKey && m_accountId == accountId;
        m_settings = std::move(settings);
        m_accountId = std::move(accountId);
        m_mailboxIds = std::move(mailboxIds);
        m_notificationMailboxIds = std::move(notificationMailboxIds);
        m_notificationMailboxSelectionConfigured = notificationMailboxSelectionConfigured;

        if (connectionSettingsUnchanged && m_runContext != nullptr)
        {
            const auto updatedConfiguration = resolveConfiguration();
            if (updatedConfiguration.has_value() &&
                updatedConfiguration->accountId == m_runContext->configuration.accountId &&
                updatedConfiguration->apiUrl == m_runContext->configuration.apiUrl &&
                updatedConfiguration->eventSourceUrl ==
                    m_runContext->configuration.eventSourceUrl &&
                updatedConfiguration->calendarCapable ==
                    m_runContext->configuration.calendarCapable &&
                updatedConfiguration->websocket.has_value() ==
                    m_runContext->configuration.websocket.has_value() &&
                (!updatedConfiguration->websocket.has_value() ||
                 (updatedConfiguration->websocket->url ==
                      m_runContext->configuration.websocket->url &&
                  updatedConfiguration->websocket->supportsPush ==
                      m_runContext->configuration.websocket->supportsPush)))
            {
                m_runContext->configuration.mailboxes = updatedConfiguration->mailboxes;
                m_runContext->configuration.notificationMailboxIds =
                    updatedConfiguration->notificationMailboxIds;
                QStringList mailboxNames;
                for (const auto& mailbox : updatedConfiguration->mailboxes)
                {
                    mailboxNames.push_back(QString::fromStdString(mailbox.second));
                }
                qInfo().noquote() << "Update watched mailboxes to"
                                  << mailboxNames.join(QStringLiteral(", "));
                scheduleDebouncedRefresh();
                return;
            }
        }

        restart();
        if (resumingAfterAuthentication && m_runContext != nullptr)
            m_shouldCatchUpRefreshOnReconnect = true;
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

        m_pendingStateChanges.clear();
        m_refreshDebounceTimer.stop();
        m_refreshInFlight = false;
        m_refreshAgainRequested = false;
        setStatus(Status::Disconnected);
        m_shouldCatchUpRefreshOnReconnect = false;
    }

    void AccountSyncCoordinator::pauseForAuthentication()
    {
        stop();
        setStatus(Status::AuthenticationPaused);
    }

    AccountSyncCoordinator::Status AccountSyncCoordinator::status() const
    {
        return m_status;
    }

    bool AccountSyncCoordinator::requestSynchronization()
    {
        if (m_status == Status::AuthenticationPaused)
            return true;
        if (m_runContext == nullptr)
            return false;
        scheduleDebouncedRefresh();
        return true;
    }

    QCoro::Task<void>
    AccountSyncCoordinator::onStateChange(javelin::jmap::sync::StateChangeEvent event)
    {
        m_lastEventId = event.newState;
        bool calendarChanged = false;
        for (auto& [type, state] : event.changedStates)
        {
            if (type == "Calendar" || type == "CalendarEvent")
            {
                calendarChanged = true;
                continue;
            }
            m_pendingStateChanges.insert_or_assign(std::move(type), std::move(state));
        }
        if (calendarChanged)
            Q_EMIT calendarStateChanged(QString::fromStdString(m_accountId));
        if (!m_pendingStateChanges.empty())
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

        auto notificationMailboxIds = m_notificationMailboxIds;
        if (!m_notificationMailboxSelectionConfigured)
        {
            const auto inbox = std::ranges::find(*mailboxTree, std::optional<std::string>{"inbox"},
                                                 &javelin::jmap::cache::MailboxTreeItem::role);
            if (inbox != mailboxTree->end())
            {
                notificationMailboxIds.push_back(inbox->id);
                if (std::ranges::find(m_mailboxIds, inbox->id) == m_mailboxIds.end())
                {
                    mailboxes.emplace_back(inbox->id, inbox->name);
                }
            }
        }

        bool calendarCapable = false;
        for (const auto& [accountId, account] : session->value().accounts)
        {
            Q_UNUSED(accountId);
            calendarCapable = calendarCapable || account.accountCapabilities.calendars.has_value();
        }

        return RunConfiguration{
            .settings = *m_settings,
            .accountId = m_accountId,
            .mailboxes = std::move(mailboxes),
            .notificationMailboxIds = {notificationMailboxIds.begin(),
                                       notificationMailboxIds.end()},
            .apiUrl = session->value().apiUrl,
            .eventSourceUrl = session->value().eventSourceUrl.value_or(std::string{}),
            .websocket = session->value().capabilities.websocket,
            .calendarCapable = calendarCapable,
        };
    }

    QCoro::Task<void> AccountSyncCoordinator::runLoop(std::shared_ptr<RunContext> runContext)
    {
        std::vector<std::string> types{"Email", "Mailbox"};
        if (runContext->configuration.calendarCapable)
        {
            types.emplace_back("Calendar");
            types.emplace_back("CalendarEvent");
        }
        javelin::jmap::sync::StateChangeSubscription subscription{
            .accountId = runContext->configuration.accountId,
            .lastState = m_lastEventId,
            .types = std::move(types),
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

        const ForegroundWorkScope foreground{m_workScheduler};
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

        const bool mailboxStateChanged = co_await refreshMailboxStateOnce(runContext);
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
        std::vector<MailboxQueryWindowChange> materializedWindows;
        bool hasNewMail = false;
        const auto watchedMailboxes = runContext->configuration.mailboxes;
        for (const auto& [mailboxId, mailboxName] : watchedMailboxes)
        {
            const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                runContext->configuration.accountId, mailboxId, {});
            if (const auto* summary =
                    std::get_if<javelin::jmap::sync::MailboxRefreshSummary>(&refreshResult))
            {
                if (summary->superseded)
                {
                    m_shouldCatchUpRefreshOnReconnect = true;
                    continue;
                }
                m_shouldCatchUpRefreshOnReconnect = false;
                watchedMailboxRefreshed = true;
                refreshedMailboxIds.push_back(QString::fromStdString(mailboxId));
                if (summary->canonicalWindowMaterialized)
                {
                    materializedWindows.push_back(MailboxQueryWindowChange{
                        .mailboxId = QString::fromStdString(mailboxId),
                        .offset = 0,
                        .limit = 100,
                        .total = std::nullopt,
                    });
                }
                hasNewMail = hasNewMail || !summary->insertedEmailIds.empty();
                if (runContext->configuration.notificationMailboxIds.contains(mailboxId))
                {
                    javelin::jmap::cache::NotificationRepository notifications{
                        m_databaseConnection};
                    const auto candidates = notifications.enqueueUnreadMailboxEmails(
                        runContext->configuration.accountId, mailboxId);
                    if (const auto* error =
                            std::get_if<javelin::jmap::cache::DatabaseError>(&candidates))
                    {
                        qWarning().noquote() << "Notification observation failed" << error->message;
                    }
                    else
                    {
                        const auto& pending = std::get<
                            std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
                            candidates);
                        publishNotifications(*runContext, mailboxId, mailboxName, pending);
                        std::vector<std::string> deliveredIds;
                        deliveredIds.reserve(pending.size());
                        for (const auto& candidate : pending)
                            deliveredIds.push_back(candidate.emailId);
                        if (const auto deliveryError = notifications.markDelivered(
                                runContext->configuration.accountId, mailboxId, deliveredIds))
                        {
                            qWarning().noquote() << "Notification delivery recording failed"
                                                 << deliveryError->message;
                        }
                    }
                }
            }
            else if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
            {
                qWarning().noquote() << "Push mailbox refresh failed" << error->message;
                handleOperationError(QStringLiteral("Synchronize mailbox"), *error);
                if (javelin::jmap::isAuthenticationError(*error))
                    co_return;
                if (javelin::jmap::isTransientError(*error))
                    scheduleDebouncedRefresh();
            }
        }
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        if (mailboxStateChanged || watchedMailboxRefreshed)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(runContext->configuration.accountId),
                .mailboxIds = std::move(refreshedMailboxIds),
                .queryWindows = std::move(materializedWindows),
                .searchWindows = {},
                .mailboxTreeChanged = mailboxStateChanged,
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

        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
        {
            qWarning().noquote() << "Account sync mailbox state refresh failed" << error->message;
            handleOperationError(QStringLiteral("Synchronize mailbox state"), *error);
            co_return false;
        }

        co_return std::get<javelin::jmap::sync::MailboxStateRefreshSummary>(refreshResult).changed;
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
        if (!m_pendingStateChanges.empty())
        {
            if (m_refreshInFlight)
            {
                m_refreshDebounceTimer.start();
                return;
            }
            if (pendingStateChangesAlreadyApplied() && watchedMailboxCoverageIsAuthoritative())
            {
                m_pendingStateChanges.clear();
                return;
            }
            m_pendingStateChanges.clear();
        }
        auto task = refreshWatchedMailbox();
        QCoro::connect(std::move(task), this, []() {});
    }

    bool AccountSyncCoordinator::pendingStateChangesAlreadyApplied() const
    {
        javelin::jmap::cache::SyncStateRepository states{m_databaseConnection};
        for (const auto& [type, advertisedState] : m_pendingStateChanges)
        {
            const auto cached =
                states.find({.accountId = m_accountId, .objectType = type, .queryKey = {}});
            const auto* record =
                std::get_if<std::optional<javelin::jmap::cache::SyncStateRecord>>(&cached);
            if (record == nullptr || !record->has_value() ||
                record->value().stateToken != advertisedState)
            {
                return false;
            }
        }
        return true;
    }

    bool AccountSyncCoordinator::watchedMailboxCoverageIsAuthoritative() const
    {
        if (m_runContext == nullptr)
        {
            return false;
        }

        std::vector<std::string> mailboxIds;
        mailboxIds.reserve(m_runContext->configuration.mailboxes.size());
        for (const auto& [mailboxId, mailboxName] : m_runContext->configuration.mailboxes)
        {
            static_cast<void>(mailboxName);
            mailboxIds.push_back(mailboxId);
        }
        const auto result = hasAuthoritativeCanonicalMailboxCoverage(
            m_databaseConnection, m_runContext->configuration.accountId, mailboxIds);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            qWarning().noquote() << "Could not inspect synchronized mailbox coverage"
                                 << error->message;
            return false;
        }
        return std::get<bool>(result);
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
            auto webSocketSource =
                std::make_unique<javelin::jmap::sync::WebSocketStateChangeSource>(
                    nextConfiguration->websocket->url, nextConfiguration->settings.apiKey,
                    sourceStatusCallback);
            std::unique_ptr<javelin::jmap::sync::StateChangeSource> httpFallbackSource;
            if (!nextConfiguration->eventSourceUrl.empty())
            {
                httpFallbackSource =
                    std::make_unique<javelin::jmap::sync::EventSourceStateChangeSource>(
                        m_networkAccessManager, nextConfiguration->eventSourceUrl,
                        nextConfiguration->settings.apiKey, sourceStatusCallback);
            }
            runContext->source = std::make_unique<javelin::jmap::sync::PreferredStateChangeSource>(
                m_transportCooldowns, nextConfiguration->websocket->url, std::move(webSocketSource),
                std::move(httpFallbackSource));
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
            },
            [this, generation = runContext->generation](const javelin::jmap::OperationError& error)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                    return;
                handleOperationError(QStringLiteral("Maintain account connection"), error);
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
        scheduleDebouncedRefresh();
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
            scheduleDebouncedRefresh();
        }
        Q_EMIT statusChanged(m_status);
    }

    void AccountSyncCoordinator::handleOperationError(const QString& operation,
                                                      const javelin::jmap::OperationError& error)
    {
        Q_EMIT operationFailed(operation, error);
        if (javelin::jmap::isAuthenticationError(error))
            pauseForAuthentication();
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

        const auto& target = candidates.front();

        QString title;
        QString message;
        if (candidates.size() == 1)
        {
            title = QStringLiteral("New mail in %1")
                        .arg(QString::fromStdString(std::string{mailboxName}));
            message = QString::fromStdString(
                candidates.front().subject.value_or(std::string{"(no subject)"}));
        }
        else
        {
            title = QStringLiteral("%1 new messages in %2")
                        .arg(candidates.size())
                        .arg(QString::fromStdString(std::string{mailboxName}));
            message = QString::fromStdString(
                candidates.front().subject.value_or(std::string{"(no subject)"}));
        }

        Q_EMIT notificationRaised(QString::fromStdString(runContext.configuration.accountId),
                                  QString::fromStdString(std::string{mailboxId}),
                                  QString::fromStdString(target.threadId),
                                  QString::fromStdString(target.emailId),
                                  QString::fromStdString(std::string{mailboxName}), title, message);
    }

} // namespace javelin::app
