#include "app/LongPollService.h"

#include "app/StateChangePolicy.h"
#include "app/WorkScheduler.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/Session.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/MailDeltaRefreshExecutor.h"
#include "jmap/sync/MailboxRefreshExecutor.h"
#include "jmap/sync/MailboxStateRefreshExecutor.h"
#include "jmap/sync/MutationJournal.h"
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
                updatedConfiguration->contactsCapable ==
                    m_runContext->configuration.contactsCapable &&
                updatedConfiguration->groupwareAccountIds ==
                    m_runContext->configuration.groupwareAccountIds &&
                updatedConfiguration->websocket.has_value() ==
                    m_runContext->configuration.websocket.has_value() &&
                (!updatedConfiguration->websocket.has_value() ||
                 (updatedConfiguration->websocket->url ==
                      m_runContext->configuration.websocket->url &&
                  updatedConfiguration->websocket->supportsPush ==
                      m_runContext->configuration.websocket->supportsPush)))
            {
                const bool watchedMailboxesChanged =
                    m_runContext->configuration.mailboxes != updatedConfiguration->mailboxes;
                const bool notificationMailboxesChanged =
                    m_runContext->configuration.notificationMailboxIds !=
                    updatedConfiguration->notificationMailboxIds;
                m_runContext->configuration.mailboxes = updatedConfiguration->mailboxes;
                m_runContext->configuration.notificationMailboxIds =
                    updatedConfiguration->notificationMailboxIds;
                if (!watchedMailboxesChanged && !notificationMailboxesChanged)
                {
                    return;
                }
                QStringList mailboxNames;
                for (const auto& mailbox : updatedConfiguration->mailboxes)
                {
                    mailboxNames.push_back(QString::fromStdString(mailbox.second));
                }
                qInfo().noquote() << "Update watched mailboxes to"
                                  << mailboxNames.join(QStringLiteral(", "));
                scheduleDebouncedRefresh(true);
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
        m_pendingCalendarStateChanges.clear();
        m_pendingContactStateChanges.clear();
        m_refreshDebounceTimer.stop();
        m_refreshInFlight = false;
        m_refreshAgainRequested = false;
        m_refreshMailboxAgainRequested = false;
        m_refreshEmailAgainRequested = false;
        m_refreshAllMailboxesAgainRequested = false;
        m_forceEmailRefreshRequested = false;
        setStatus(Status::Disconnected);
        m_shouldCatchUpRefreshOnReconnect = false;
    }

    void AccountSyncCoordinator::pauseForAuthentication()
    {
        stop();
        setStatus(Status::AuthenticationPaused);
    }

    bool AccountSyncCoordinator::requestSynchronization()
    {
        if (m_status == Status::AuthenticationPaused)
            return true;
        if (m_runContext == nullptr)
            return false;
        scheduleDebouncedRefresh(true);
        return true;
    }

    QCoro::Task<void>
    AccountSyncCoordinator::onStateChange(javelin::jmap::sync::StateChangeEvent event)
    {
        m_lastEventId = event.newState;
        auto routed = routeStateChanges(std::move(event.changedStates), m_accountId);
        for (auto& [type, state] : routed.mailStates)
            m_pendingStateChanges.insert_or_assign(std::move(type), std::move(state));
        const auto merge = [](auto& destination, auto source)
        {
            for (auto& [accountId, states] : source)
                for (auto& [type, state] : states)
                    destination[accountId].insert_or_assign(std::move(type), std::move(state));
        };
        merge(m_pendingCalendarStateChanges, std::move(routed.calendarStates));
        merge(m_pendingContactStateChanges, std::move(routed.contactStates));
        if (!m_pendingStateChanges.empty() || !m_pendingCalendarStateChanges.empty() ||
            !m_pendingContactStateChanges.empty())
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
        bool contactsCapable = false;
        std::vector<std::string> groupwareAccountIds;
        for (const auto& [accountId, account] : session->value().accounts)
        {
            const bool accountHasCalendar = account.accountCapabilities.calendars.has_value();
            const bool accountHasContacts = account.accountCapabilities.contacts.has_value();
            calendarCapable = calendarCapable || accountHasCalendar;
            contactsCapable = contactsCapable || accountHasContacts;
            if (accountHasCalendar || accountHasContacts)
                groupwareAccountIds.push_back(accountId);
        }
        std::ranges::sort(groupwareAccountIds);

        const auto requestLimits = javelin::jmap::api::coreRequestLimits(session->value());
        if (!requestLimits.has_value())
        {
            qWarning() << "Account sync configuration unavailable because the cached session has "
                          "invalid JMAP Core request limits";
            return std::nullopt;
        }

        return RunConfiguration{
            .settings = *m_settings,
            .accountId = m_accountId,
            .mailboxes = std::move(mailboxes),
            .notificationMailboxIds = {notificationMailboxIds.begin(),
                                       notificationMailboxIds.end()},
            .apiUrl = session->value().apiUrl,
            .requestLimits = *requestLimits,
            .eventSourceUrl = session->value().eventSourceUrl.value_or(std::string{}),
            .websocket = session->value().capabilities.websocket,
            .groupwareAccountIds = std::move(groupwareAccountIds),
            .calendarCapable = calendarCapable,
            .contactsCapable = contactsCapable,
        };
    }

    QCoro::Task<void> AccountSyncCoordinator::runLoop(std::shared_ptr<RunContext> runContext)
    {
        auto types =
            subscribedStateChangeTypes({.calendar = runContext->configuration.calendarCapable,
                                        .contacts = runContext->configuration.contactsCapable});
        javelin::jmap::sync::StateChangeSubscription subscription{
            .accountId = runContext->configuration.accountId,
            .lastState = m_lastEventId,
            .types = std::move(types),
            .groupwareAccountIds = runContext->configuration.groupwareAccountIds,
        };

        co_await runContext->worker->run(subscription, runContext->cancellation);
    }

    QCoro::Task<void> AccountSyncCoordinator::refreshWatchedMailbox(const bool refreshMailboxState,
                                                                    const bool refreshEmailState,
                                                                    const bool refreshAllMailboxes)
    {
        auto runContext = m_runContext;
        if (runContext == nullptr || !hasValidSettings())
        {
            co_return;
        }

        if (m_refreshInFlight)
        {
            m_refreshAgainRequested = true;
            m_refreshMailboxAgainRequested = m_refreshMailboxAgainRequested || refreshMailboxState;
            m_refreshEmailAgainRequested = m_refreshEmailAgainRequested || refreshEmailState;
            m_refreshAllMailboxesAgainRequested =
                m_refreshAllMailboxesAgainRequested || refreshAllMailboxes;
            co_return;
        }

        const ForegroundWorkScope foreground{m_workScheduler};
        m_refreshInFlight = true;
        const auto generation = runContext->generation;
        bool refreshMailbox = refreshMailboxState;
        bool refreshEmail = refreshEmailState;
        bool refreshAll = refreshAllMailboxes;
        do
        {
            m_refreshAgainRequested = false;
            m_refreshMailboxAgainRequested = false;
            m_refreshEmailAgainRequested = false;
            m_refreshAllMailboxesAgainRequested = false;
            co_await refreshWatchedMailboxOnce(runContext, refreshMailbox, refreshEmail,
                                               refreshAll);
            runContext = m_runContext;
            refreshMailbox = m_refreshMailboxAgainRequested;
            refreshEmail = m_refreshEmailAgainRequested;
            refreshAll = m_refreshAllMailboxesAgainRequested;
        } while (m_refreshAgainRequested && runContext != nullptr &&
                 runContext->generation == generation && !runContext->cancellation.isCancelled());

        if (m_runContext == nullptr || m_runContext->generation == generation)
        {
            m_refreshInFlight = false;
        }
    }

    QCoro::Task<void> AccountSyncCoordinator::refreshWatchedMailboxOnce(
        std::shared_ptr<RunContext> runContext, const bool refreshMailboxState,
        const bool refreshEmailState, const bool refreshAllMailboxes)
    {
        if (runContext == nullptr || !hasValidSettings() ||
            runContext->cancellation.isCancelled() || m_runContext == nullptr ||
            m_runContext->generation != runContext->generation)
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
            .requestLimits = runContext->configuration.requestLimits,
        };

        bool mailboxStateChanged = false;
        bool emailCacheChanged = false;
        bool refreshEveryMailbox = refreshAllMailboxes;
        bool accountEmailStateRefreshed = false;
        bool hasNewMail = false;
        std::vector<std::string> queryAffectedMailboxIds;
        QStringList refreshedMailboxIds;

        if (refreshAllMailboxes)
        {
            mailboxStateChanged = co_await refreshMailboxStateOnce(runContext);
        }
        else if (refreshMailboxState || refreshEmailState)
        {
            javelin::jmap::sync::MailDeltaRefreshExecutor deltaExecutor{
                m_databaseConnection, methodCaller, apiRequestContext};
            const auto deltaResult = co_await deltaExecutor.refresh(
                runContext->configuration.accountId, {
                                                         .mailbox = refreshMailboxState,
                                                         .email = refreshEmailState,
                                                     });
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&deltaResult))
            {
                qWarning().noquote() << "Account mail delta refresh failed" << error->message;
                handleOperationError(QStringLiteral("Synchronize mail changes"), *error);
                if (javelin::jmap::isTransientError(*error))
                    scheduleDebouncedRefresh(true);
                co_return;
            }
            const auto& delta = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(deltaResult);
            if (delta.superseded)
            {
                m_refreshAgainRequested = true;
                m_refreshMailboxAgainRequested =
                    m_refreshMailboxAgainRequested || refreshMailboxState;
                m_refreshEmailAgainRequested = m_refreshEmailAgainRequested || refreshEmailState;
                co_return;
            }
            mailboxStateChanged = delta.mailboxChanged;
            emailCacheChanged = delta.emailChanged;
            accountEmailStateRefreshed = refreshEmailState && !delta.emailNeedsFullRefresh;
            refreshEveryMailbox = delta.emailNeedsFullRefresh;
            queryAffectedMailboxIds = delta.queryAffectedMailboxIds;
            hasNewMail = !delta.insertedEmailIds.empty();
            for (const auto& mailboxId : delta.changedMailboxIds)
                refreshedMailboxIds.push_back(QString::fromStdString(mailboxId));
            if (delta.mailboxNeedsFullRefresh)
                mailboxStateChanged = co_await refreshMailboxStateOnce(runContext);
        }

        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        if (!refreshEmailState && !refreshEveryMailbox)
        {
            if (mailboxStateChanged)
            {
                Q_EMIT cacheCommitted(MailCacheChange{
                    .accountId = QString::fromStdString(runContext->configuration.accountId),
                    .mailboxIds = std::move(refreshedMailboxIds),
                    .queryWindows = {},
                    .searchWindows = {},
                    .mailboxTreeChanged = true,
                    .hasNewMail = false,
                });
            }
            co_return;
        }

        javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
            m_databaseConnection, methodCaller, apiRequestContext};
        bool watchedMailboxRefreshed = false;
        std::vector<MailboxQueryWindowChange> materializedWindows;
        const auto watchedMailboxes = runContext->configuration.mailboxes;
        for (const auto& [mailboxId, mailboxName] : watchedMailboxes)
        {
            const bool refreshForNotification =
                hasNewMail &&
                runContext->configuration.notificationMailboxIds.contains(mailboxId) &&
                std::ranges::find(queryAffectedMailboxIds, mailboxId) !=
                    queryAffectedMailboxIds.end();
            if (!refreshEveryMailbox && !refreshForNotification)
                continue;
            const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                runContext->configuration.accountId, mailboxId, {}, false,
                !accountEmailStateRefreshed);
            if (const auto* summary =
                    std::get_if<javelin::jmap::sync::MailboxRefreshSummary>(&refreshResult))
            {
                if (summary->superseded)
                {
                    m_shouldCatchUpRefreshOnReconnect = true;
                    continue;
                }
                m_shouldCatchUpRefreshOnReconnect = false;
                accountEmailStateRefreshed =
                    accountEmailStateRefreshed || summary->usedIncrementalRefresh;
                watchedMailboxRefreshed = true;
                const auto qMailboxId = QString::fromStdString(mailboxId);
                if (!refreshedMailboxIds.contains(qMailboxId))
                    refreshedMailboxIds.push_back(qMailboxId);
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
                    scheduleDebouncedRefresh(true);
            }
        }
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        if (mailboxStateChanged || emailCacheChanged || watchedMailboxRefreshed)
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
            .requestLimits = runContext->configuration.requestLimits,
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
            if (javelin::jmap::isTransientError(*error))
                scheduleDebouncedRefresh(true);
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

    void AccountSyncCoordinator::scheduleDebouncedRefresh(const bool forceEmailRefresh)
    {
        if (!hasValidSettings() || m_runContext == nullptr)
        {
            return;
        }

        m_forceEmailRefreshRequested = m_forceEmailRefreshRequested || forceEmailRefresh;
        m_refreshDebounceTimer.start();
    }

    void AccountSyncCoordinator::scheduleCatchUpRefresh()
    {
        processGroupwareStateChanges();
        if (m_pendingStateChanges.empty() && !m_forceEmailRefreshRequested)
            return;

        if (!m_pendingStateChanges.empty())
        {
            if (m_refreshInFlight)
            {
                m_refreshDebounceTimer.start();
                return;
            }
            if (pendingStateChangesAlreadyApplied() && !m_forceEmailRefreshRequested)
            {
                m_pendingStateChanges.clear();
                return;
            }
        }

        bool refreshMailboxState = m_forceEmailRefreshRequested;
        bool refreshEmailState = m_forceEmailRefreshRequested;
        const bool refreshAllMailboxes = m_forceEmailRefreshRequested;
        if (const auto mailbox = m_pendingStateChanges.find("Mailbox");
            mailbox != m_pendingStateChanges.end() &&
            !pendingStateChangeAlreadyApplied(mailbox->first, mailbox->second))
        {
            refreshMailboxState = true;
        }
        if (const auto email = m_pendingStateChanges.find("Email");
            email != m_pendingStateChanges.end() &&
            !pendingStateChangeAlreadyApplied(email->first, email->second))
        {
            refreshEmailState = true;
        }
        m_forceEmailRefreshRequested = false;
        m_pendingStateChanges.clear();
        auto task =
            refreshWatchedMailbox(refreshMailboxState, refreshEmailState, refreshAllMailboxes);
        QCoro::connect(std::move(task), this, []() {});
    }

    bool AccountSyncCoordinator::stateChangeAlreadyApplied(const std::string_view accountId,
                                                           const std::string_view type,
                                                           const std::string_view state) const
    {
        javelin::jmap::cache::SyncStateRepository states{m_databaseConnection};
        const auto cached = states.find(
            {.accountId = std::string{accountId}, .objectType = std::string{type}, .queryKey = {}});
        const auto* record =
            std::get_if<std::optional<javelin::jmap::cache::SyncStateRecord>>(&cached);
        return record != nullptr && record->has_value() && record->value().stateToken == state;
    }

    bool AccountSyncCoordinator::domainHasActiveMutation(const std::string_view accountId,
                                                         const std::string_view type) const
    {
        javelin::jmap::sync::MutationJournalRepository journal{m_databaseConnection};
        const auto active = journal.listActive(
            {.accountId = std::string{accountId}, .dataType = std::string{type}});
        const auto* records =
            std::get_if<std::vector<javelin::jmap::sync::MutationRecord>>(&active);
        return records == nullptr || !records->empty();
    }

    void AccountSyncCoordinator::processGroupwareStateChanges()
    {
        const auto process = [this](auto& pending, auto emitReady)
        {
            javelin::jmap::sync::AccountTypeStateMap deferred;
            javelin::jmap::sync::AccountTypeStateMap ready;
            for (auto& [accountId, states] : pending)
            {
                for (auto& [type, state] : states)
                {
                    if (domainHasActiveMutation(accountId, type))
                    {
                        deferred[accountId].insert_or_assign(type, state);
                        continue;
                    }
                    if (!stateChangeAlreadyApplied(accountId, type, state))
                        ready[accountId].insert_or_assign(type, state);
                }
            }
            pending = std::move(deferred);
            if (!ready.empty())
                emitReady(ready);
        };
        const auto ownerAccountId = QString::fromStdString(m_accountId);
        process(m_pendingCalendarStateChanges, [this, &ownerAccountId](const auto& states)
                { Q_EMIT calendarStateChanged(ownerAccountId, states); });
        process(m_pendingContactStateChanges, [this, &ownerAccountId](const auto& states)
                { Q_EMIT contactStateChanged(ownerAccountId, states); });
        if (!m_pendingCalendarStateChanges.empty() || !m_pendingContactStateChanges.empty())
            m_refreshDebounceTimer.start();
    }

    bool
    AccountSyncCoordinator::pendingStateChangeAlreadyApplied(const std::string_view type,
                                                             const std::string_view state) const
    {
        return stateChangeAlreadyApplied(m_accountId, type, state);
    }

    bool AccountSyncCoordinator::pendingStateChangesAlreadyApplied() const
    {
        for (const auto& [type, advertisedState] : m_pendingStateChanges)
        {
            if (!pendingStateChangeAlreadyApplied(type, advertisedState))
            {
                return false;
            }
        }
        return true;
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
            scheduleDebouncedRefresh(true);
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
        scheduleDebouncedRefresh(true);
    }

    void AccountSyncCoordinator::setStatus(const Status status)
    {
        if (m_status == status)
        {
            if (status == Status::Disconnected && m_shouldCatchUpRefreshOnReconnect)
            {
                scheduleDebouncedRefresh(true);
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
            scheduleDebouncedRefresh(true);
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
