#include "app/account/AccountSyncCoordinator.h"

#include "app/StateChangePolicy.h"
#include "app/WorkScheduler.h"

#include "jmap/api/MethodCaller.h"
#include "jmap/api/Session.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/sync/ConsistencyDomain.h"
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
        constexpr unsigned int notificationBaselineRetryMaximumExponent = 5;
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
        javelin::jmap::cache::MailboxReader& mailboxReader, WorkScheduler& workScheduler,
        EndpointRetryGate& endpointRetryGate,
        javelin::jmap::auth::AccessTokenRefreshHandler authenticationRefreshHandler,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection),
          m_methodTransport(methodTransport), m_networkAccessManager(networkAccessManager),
          m_transportCooldowns(cooldowns), m_accountRepository(accountRepository),
          m_mailboxReader(mailboxReader), m_workScheduler(workScheduler),
          m_endpointRetryGate(endpointRetryGate),
          m_authenticationRefreshHandler(std::move(authenticationRefreshHandler))
    {
        m_refreshDebounceTimer.setSingleShot(true);
        m_refreshDebounceTimer.setInterval(refreshDebounceInterval);
        QObject::connect(&m_refreshDebounceTimer, &QTimer::timeout, this,
                         &AccountSyncCoordinator::scheduleCatchUpRefresh);
        m_notificationBaselineRetryTimer.setSingleShot(true);
        QObject::connect(&m_notificationBaselineRetryTimer, &QTimer::timeout, this,
                         &AccountSyncCoordinator::scheduleNotificationBaselineRefresh);
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

    AccountSyncCoordinator::Status AccountSyncCoordinator::status() const
    {
        return m_status;
    }

    void AccountSyncCoordinator::applySettings(AccountConnectionSettings settings,
                                               std::string accountId,
                                               std::vector<std::string> mailboxIds,
                                               std::vector<std::string> notificationMailboxIds)
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

        if (connectionSettingsUnchanged && m_runContext != nullptr)
        {
            const auto updatedConfiguration = resolveConfiguration();
            if (updatedConfiguration.has_value() &&
                updatedConfiguration->accountId == m_runContext->configuration.accountId &&
                updatedConfiguration->remoteAccountId ==
                    m_runContext->configuration.remoteAccountId &&
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
                auto newlyAddedMailboxIds = newlyWatchedMailboxIds(
                    m_runContext->configuration.mailboxes, updatedConfiguration->mailboxes);
                m_runContext->configuration.mailboxes = updatedConfiguration->mailboxes;
                m_runContext->configuration.notificationMailboxIds =
                    updatedConfiguration->notificationMailboxIds;
                if (!watchedMailboxesChanged && !notificationMailboxesChanged)
                {
                    return;
                }
                if (watchedMailboxesChanged)
                    qInfo() << "Update watched mailbox selection"
                            << "count=" << updatedConfiguration->mailboxes.size();
                if (!newlyAddedMailboxIds.empty())
                    scheduleDebouncedRefresh(false, std::move(newlyAddedMailboxIds));
                return;
            }
        }

        restart();
        if (resumingAfterAuthentication && m_runContext != nullptr)
            m_shouldCatchUpRefreshOnReconnect = true;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    AccountSyncCoordinator::requestNotificationBaseline(std::vector<std::string> mailboxIds)
    {
        std::ranges::sort(mailboxIds);
        mailboxIds.erase(std::ranges::unique(mailboxIds).begin(), mailboxIds.end());
        if (m_pendingNotificationBaselineMailboxIds == mailboxIds)
            return std::nullopt;

        javelin::jmap::sync::ConsistencyDomainRepository consistency{m_databaseConnection};
        const auto generation =
            consistency.advanceMutation({.accountId = m_accountId, .dataType = "Email"});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generation))
            return *error;

        m_pendingNotificationBaselineMailboxIds = std::move(mailboxIds);
        m_notificationBaselineRetryAttempts = 0;
        m_notificationBaselineRetryTimer.stop();
        scheduleNotificationBaselineRefresh();
        return std::nullopt;
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    AccountSyncCoordinator::cancelNotificationBaseline()
    {
        if (!m_pendingNotificationBaselineMailboxIds.has_value())
            return std::nullopt;

        javelin::jmap::sync::ConsistencyDomainRepository consistency{m_databaseConnection};
        const auto generation =
            consistency.advanceMutation({.accountId = m_accountId, .dataType = "Email"});
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&generation))
            return *error;

        m_pendingNotificationBaselineMailboxIds.reset();
        m_notificationBaselineRetryAttempts = 0;
        m_notificationBaselineRetryTimer.stop();
        return std::nullopt;
    }

    void AccountSyncCoordinator::stop()
    {
        if (m_runContext != nullptr)
        {
            m_runContext->cancellation.cancel();
            if (m_runContext->source != nullptr)
            {
                m_runContext->source->cancel();
            }
            m_runContext->sleeper.cancel();
            m_runContext.reset();
        }

        m_pendingStateChanges.clear();
        m_pendingCalendarStateChanges.clear();
        m_pendingContactStateChanges.clear();
        m_pendingIdentityStateChanges.clear();
        m_authenticationRecoveryInFlight = false;
        m_refreshDebounceTimer.stop();
        m_notificationBaselineRetryTimer.stop();
        m_queuedRefreshDemand = {};
        m_debouncedRefreshDemand = {};
        setStatus(Status::Disconnected);
        m_shouldCatchUpRefreshOnReconnect = false;
    }

    void AccountSyncCoordinator::pauseForAuthentication()
    {
        stop();
        setStatus(Status::AuthenticationPaused);
    }

    void AccountSyncCoordinator::networkBecameReachable()
    {
        if (m_runContext != nullptr)
            m_endpointRetryGate.reset(m_runContext->configuration.apiUrl);
        restartForCatchUp();
    }

    bool AccountSyncCoordinator::requestSynchronization()
    {
        if (m_status == Status::AuthenticationPaused)
            return true;
        if (m_runContext == nullptr)
            return false;
        m_endpointRetryGate.reset(m_runContext->configuration.apiUrl);
        scheduleDebouncedRefresh(true);
        return true;
    }

    bool AccountSyncCoordinator::requestMailboxSynchronization(const std::string_view mailboxId)
    {
        if (mailboxId.empty())
            return false;
        if (m_status == Status::AuthenticationPaused)
            return true;
        if (m_runContext == nullptr)
            return false;
        m_endpointRetryGate.reset(m_runContext->configuration.apiUrl);
        scheduleDebouncedRefresh(false, {std::string{mailboxId}});
        return true;
    }

    QCoro::Task<void>
    AccountSyncCoordinator::onStateChange(javelin::jmap::sync::StateChangeEvent event)
    {
        if (m_runContext == nullptr)
            co_return;
        m_lastEventId = event.newState;
        auto routed = routeStateChanges(std::move(event.changedStates),
                                        m_runContext->configuration.remoteAccountId);
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
        merge(m_pendingIdentityStateChanges, std::move(routed.identityStates));
        if (!m_pendingStateChanges.empty() || !m_pendingCalendarStateChanges.empty() ||
            !m_pendingContactStateChanges.empty() || !m_pendingIdentityStateChanges.empty())
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
        if (session == nullptr)
        {
            qWarning() << "Account sync configuration unavailable because the cached session "
                          "could not be loaded";
            return std::nullopt;
        }
        if (!session->has_value())
        {
            qInfo() << "Account sync is waiting for initial session discovery";
            return std::nullopt;
        }

        const auto accountResult = m_accountRepository.findById(m_accountId);
        const auto* cachedAccount =
            std::get_if<std::optional<javelin::jmap::cache::CachedAccount>>(&accountResult);
        if (cachedAccount == nullptr || !cachedAccount->has_value() ||
            (*cachedAccount)->remoteAccountId.empty())
        {
            qWarning() << "Account sync configuration unavailable because the remote account "
                          "identity could not be resolved";
            return std::nullopt;
        }
        const std::string remoteAccountId = (*cachedAccount)->remoteAccountId;
        if (!(*session)->accounts.contains(remoteAccountId))
        {
            qWarning() << "Account sync configuration unavailable because the remote mail account "
                          "is not present in the cached session";
            return std::nullopt;
        }

        if (!(*session)->eventSourceUrl.has_value() &&
            (!(*session)->capabilities.websocket.has_value() ||
             !(*session)->capabilities.websocket->supportsPush))
        {
            qWarning() << "Account sync configuration unavailable because the cached session has "
                          "no state-change source";
            return std::nullopt;
        }

        const auto mailboxTreeResult = m_mailboxReader.listMailboxTree(m_accountId);
        const auto* mailboxTree =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxTreeResult);
        if (mailboxTree == nullptr || mailboxTree->empty())
        {
            qWarning() << "Account sync configuration unavailable because the mailbox tree is "
                          "empty";
            return std::nullopt;
        }

        std::vector<std::pair<std::string, std::string>> mailboxes;
        std::unordered_set<std::string> subscribedMailboxIds;
        for (const auto& mailbox : *mailboxTree)
        {
            if (!mailbox.isSubscribed)
                continue;
            subscribedMailboxIds.insert(mailbox.id);
            if (std::ranges::find(m_mailboxIds, mailbox.id) != m_mailboxIds.end())
                mailboxes.emplace_back(mailbox.id, mailbox.name);
        }
        std::unordered_set<std::string> notificationMailboxIds;
        for (const auto& mailboxId : m_notificationMailboxIds)
        {
            if (subscribedMailboxIds.contains(mailboxId))
                notificationMailboxIds.insert(mailboxId);
        }

        bool calendarCapable = false;
        bool contactsCapable = false;
        bool identitiesCapable = false;
        std::vector<std::string> groupwareAccountIds;
        for (const auto& [accountId, account] : session->value().accounts)
        {
            const bool accountHasCalendar = account.accountCapabilities.calendars.has_value();
            const bool accountHasContacts = account.accountCapabilities.contacts.has_value();
            const bool accountHasIdentities = account.accountCapabilities.submission.has_value();
            calendarCapable = calendarCapable || accountHasCalendar;
            contactsCapable = contactsCapable || accountHasContacts;
            identitiesCapable = identitiesCapable || accountHasIdentities;
            if (accountHasCalendar || accountHasContacts || accountHasIdentities)
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
            .remoteAccountId = std::move(remoteAccountId),
            .mailboxes = std::move(mailboxes),
            .notificationMailboxIds = std::move(notificationMailboxIds),
            .apiUrl = session->value().apiUrl,
            .requestLimits = *requestLimits,
            .eventSourceUrl = session->value().eventSourceUrl.value_or(std::string{}),
            .websocket = session->value().capabilities.websocket,
            .groupwareAccountIds = std::move(groupwareAccountIds),
            .calendarCapable = calendarCapable,
            .contactsCapable = contactsCapable,
            .identitiesCapable = identitiesCapable,
        };
    }

    QCoro::Task<void> AccountSyncCoordinator::runLoop(std::shared_ptr<RunContext> runContext)
    {
        auto types = subscribedStateChangeTypes({
            .calendar = runContext->configuration.calendarCapable,
            .contacts = runContext->configuration.contactsCapable,
            .identities = runContext->configuration.identitiesCapable,
        });
        javelin::jmap::sync::StateChangeSubscription subscription{
            .accountId = runContext->configuration.remoteAccountId,
            .lastState = m_lastEventId,
            .types = std::move(types),
            .groupwareAccountIds = runContext->configuration.groupwareAccountIds,
        };

        co_await runContext->worker->run(subscription, runContext->cancellation);
    }

    QCoro::Task<void>
    AccountSyncCoordinator::refreshWatchedMailbox(MailRefreshDemand demand,
                                                  EndpointRetryGate::Lease retryLease)
    {
        auto runContext = m_runContext;
        if (runContext == nullptr || !hasValidSettings() || demand.empty())
            co_return;

        if (m_refreshInFlight)
        {
            m_queuedRefreshDemand.merge(demand);
            co_return;
        }

        const ForegroundWorkScope foreground{m_workScheduler};
        m_refreshInFlight = true;
        const auto generation = runContext->generation;
        m_refreshGenerationInFlight = generation;
        do
        {
            co_await refreshWatchedMailboxOnce(runContext, demand, retryLease);
            runContext = m_runContext;
            if (runContext == nullptr || runContext->generation != generation ||
                runContext->cancellation.isCancelled())
            {
                break;
            }
            demand = std::exchange(m_queuedRefreshDemand, {});
        } while (!demand.empty());

        if (m_refreshGenerationInFlight == generation)
        {
            m_refreshInFlight = false;
            m_refreshGenerationInFlight.reset();
            if (!m_queuedRefreshDemand.empty() && m_runContext != nullptr &&
                !m_runContext->cancellation.isCancelled())
            {
                auto queued = std::exchange(m_queuedRefreshDemand, {});
                auto queuedLease = m_endpointRetryGate.acquire(m_runContext->configuration.apiUrl);
                if (!queuedLease.allowed())
                {
                    m_debouncedRefreshDemand.merge(queued);
                    m_refreshDebounceTimer.start(static_cast<int>(
                        std::max<std::int64_t>(1, queuedLease.retryAfter().count())));
                }
                else
                {
                    auto task = refreshWatchedMailbox(queued, std::move(queuedLease));
                    QCoro::connect(std::move(task), this, []() {});
                }
            }
        }
    }

    QCoro::Task<void>
    AccountSyncCoordinator::refreshWatchedMailboxOnce(std::shared_ptr<RunContext> runContext,
                                                      const MailRefreshDemand demand,
                                                      EndpointRetryGate::Lease& retryLease)
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
        bool endpointRequestSucceeded = false;
        bool refreshEveryMailbox = demand.allMailboxes;
        bool emailObjectsChanged = false;
        std::vector<std::string> queryAffectedMailboxIds;
        QStringList refreshedMailboxIds;

        if (demand.mailboxState || demand.emailState)
        {
            javelin::jmap::sync::MailDeltaRefreshExecutor deltaExecutor{
                m_databaseConnection, methodCaller, apiRequestContext};
            const auto notificationBaselineMailboxIds =
                demand.emailState ? m_pendingNotificationBaselineMailboxIds : std::nullopt;
            const auto deltaResult = co_await deltaExecutor.refresh(
                runContext->configuration.accountId,
                {.mailbox = demand.mailboxState, .email = demand.emailState},
                runContext->configuration.remoteAccountId, notificationBaselineMailboxIds);
            if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
                runContext->cancellation.isCancelled())
            {
                co_return;
            }
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&deltaResult))
            {
                qWarning().noquote() << "Account mail delta refresh failed" << error->message;
                recordRefreshFailure(retryLease, *error);
                publishOperationError(QStringLiteral("Synchronize mail changes"), *error);
                if (notificationBaselineMailboxIds.has_value() &&
                    (error->code == javelin::jmap::OperationErrorCode::LocalStorageBusy ||
                     error->code == javelin::jmap::OperationErrorCode::LocalStorageFailure))
                    scheduleNotificationBaselineRetry();
                else if (javelin::jmap::isTransientError(*error))
                    scheduleDebouncedRefresh(true);
                co_return;
            }
            endpointRequestSucceeded = true;
            const auto& delta = std::get<javelin::jmap::sync::MailDeltaRefreshSummary>(deltaResult);
            if (delta.superseded)
            {
                m_queuedRefreshDemand.merge(demand);
                co_return;
            }
            if (delta.notificationBaselineEstablished &&
                notificationBaselineMailboxIds.has_value() &&
                m_pendingNotificationBaselineMailboxIds == notificationBaselineMailboxIds)
            {
                m_pendingNotificationBaselineMailboxIds.reset();
                m_notificationBaselineRetryAttempts = 0;
                m_notificationBaselineRetryTimer.stop();
            }
            mailboxStateChanged = delta.mailboxChanged;
            emailCacheChanged = delta.emailChanged;
            refreshEveryMailbox = refreshEveryMailbox || delta.mailboxQueriesNeedReconciliation;
            queryAffectedMailboxIds = delta.queryAffectedMailboxIds;
            if (delta.notificationEventsCreated)
                Q_EMIT notificationEventsCommitted(
                    QString::fromStdString(runContext->configuration.accountId));
            emailObjectsChanged = delta.emailChanged;
            for (const auto& mailboxId : delta.changedMailboxIds)
                refreshedMailboxIds.push_back(QString::fromStdString(mailboxId));
            if (delta.mailboxNeedsFullRefresh)
            {
                const auto mailboxState = co_await refreshMailboxStateOnce(runContext, retryLease);
                if (!mailboxState.has_value())
                    co_return;
                mailboxStateChanged = *mailboxState;
                endpointRequestSucceeded = true;
            }
        }

        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return;
        }

        if (mailboxStateChanged)
        {
            const auto updatedConfiguration = resolveConfiguration();
            if (updatedConfiguration.has_value() &&
                updatedConfiguration->accountId == runContext->configuration.accountId)
            {
                runContext->configuration.mailboxes = updatedConfiguration->mailboxes;
                runContext->configuration.notificationMailboxIds =
                    updatedConfiguration->notificationMailboxIds;
            }
        }

        if (!demand.emailState && !refreshEveryMailbox && demand.mailboxIds.empty())
        {
            if (mailboxStateChanged)
            {
                Q_EMIT cacheCommitted(MailCacheChange{
                    .accountId = QString::fromStdString(runContext->configuration.accountId),
                    .mailboxIds = std::move(refreshedMailboxIds),
                    .queryWindows = {},
                    .searchWindows = {},
                    .mailboxTreeChanged = true,
                    .emailObjectsChanged = false,
                });
            }
            if (endpointRequestSucceeded)
                recordRefreshSuccess(retryLease);
            co_return;
        }

        javelin::jmap::sync::MailboxRefreshExecutor mailboxRefreshExecutor{
            m_databaseConnection, methodCaller, apiRequestContext};
        bool watchedMailboxRefreshed = false;
        std::vector<MailboxQueryWindowChange> materializedWindows;
        auto refreshMailboxes =
            mailboxRefreshTargets(runContext->configuration.mailboxes, demand.mailboxIds);
        if (!demand.mailboxIds.empty())
        {
            const auto mailboxTreeResult =
                m_mailboxReader.listMailboxTree(runContext->configuration.accountId);
            const auto* mailboxTree =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxTreeResult);
            if (mailboxTree != nullptr)
            {
                for (auto& [mailboxId, mailboxName] : refreshMailboxes)
                {
                    if (!mailboxName.empty())
                        continue;
                    if (const auto found = std::ranges::find(
                            *mailboxTree, mailboxId, &javelin::jmap::cache::MailboxTreeItem::id);
                        found != mailboxTree->end())
                        mailboxName = found->name;
                }
            }
        }
        for (const auto& [mailboxId, mailboxName] : refreshMailboxes)
        {
            if (!shouldRefreshMailboxWindow(refreshEveryMailbox, queryAffectedMailboxIds,
                                            demand.mailboxIds, mailboxId))
                continue;
            const auto refreshResult = co_await mailboxRefreshExecutor.refreshCollapsedMailbox(
                runContext->configuration.accountId, mailboxId, {}, false,
                runContext->configuration.remoteAccountId);
            if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
                runContext->cancellation.isCancelled())
            {
                co_return;
            }
            if (const auto* summary =
                    std::get_if<javelin::jmap::sync::MailboxRefreshSummary>(&refreshResult))
            {
                endpointRequestSucceeded = true;
                if (summary->superseded)
                {
                    m_shouldCatchUpRefreshOnReconnect = true;
                    continue;
                }
                m_shouldCatchUpRefreshOnReconnect = false;
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
                emailObjectsChanged = emailObjectsChanged || !summary->changedEmailIds.empty();
            }
            else if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
            {
                qWarning().noquote() << "Push mailbox refresh failed" << error->message;
                recordRefreshFailure(retryLease, *error);
                publishOperationError(QStringLiteral("Synchronize mailbox"), *error);
                if (javelin::jmap::isAuthenticationError(*error))
                    co_return;
                if (javelin::jmap::isTransientError(*error))
                {
                    scheduleDebouncedRefresh(true);
                    co_return;
                }
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
                .emailObjectsChanged = emailObjectsChanged,
            });
        }
        if (endpointRequestSucceeded)
            recordRefreshSuccess(retryLease);
    }

    QCoro::Task<std::optional<bool>>
    AccountSyncCoordinator::refreshMailboxStateOnce(std::shared_ptr<RunContext> runContext,
                                                    EndpointRetryGate::Lease& retryLease)
    {
        if (runContext == nullptr || !hasValidSettings() ||
            runContext->cancellation.isCancelled() || m_runContext == nullptr ||
            m_runContext->generation != runContext->generation)
        {
            co_return std::nullopt;
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
        const auto refreshResult = co_await executor.refresh(
            runContext->configuration.accountId, runContext->configuration.remoteAccountId);
        if (m_runContext == nullptr || m_runContext->generation != runContext->generation ||
            runContext->cancellation.isCancelled())
        {
            co_return std::nullopt;
        }

        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&refreshResult))
        {
            qWarning().noquote() << "Account sync mailbox state refresh failed" << error->message;
            recordRefreshFailure(retryLease, *error);
            publishOperationError(QStringLiteral("Synchronize mailbox state"), *error);
            if (javelin::jmap::isTransientError(*error))
                scheduleDebouncedRefresh(true);
            co_return std::nullopt;
        }

        const auto& summary =
            std::get<javelin::jmap::sync::MailboxStateRefreshSummary>(refreshResult);
        if (summary.superseded)
        {
            m_queuedRefreshDemand.merge(MailRefreshDemand{.mailboxState = true,
                                                          .emailState = false,
                                                          .allMailboxes = false,
                                                          .mailboxIds = {}});
            co_return false;
        }
        co_return summary.changed;
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

    void AccountSyncCoordinator::scheduleDebouncedRefresh(const bool forceEmailRefresh,
                                                          std::vector<std::string> mailboxIds)
    {
        if (!hasValidSettings() || m_runContext == nullptr)
        {
            return;
        }

        if (forceEmailRefresh)
            m_debouncedRefreshDemand.merge(MailRefreshDemand::full());
        else if (!mailboxIds.empty())
            m_debouncedRefreshDemand.merge(MailRefreshDemand{.mailboxState = false,
                                                             .emailState = false,
                                                             .allMailboxes = false,
                                                             .mailboxIds = std::move(mailboxIds)});
        m_refreshDebounceTimer.start(static_cast<int>(refreshDebounceInterval.count()));
    }

    void AccountSyncCoordinator::scheduleNotificationBaselineRetry()
    {
        // The runtime manager owns failures before the baseline is installed here. Once pending,
        // this coordinator retries only execution through MailDeltaRefreshExecutor.
        if (!m_pendingNotificationBaselineMailboxIds.has_value())
            return;
        const auto exponent =
            std::min(m_notificationBaselineRetryAttempts, notificationBaselineRetryMaximumExponent);
        ++m_notificationBaselineRetryAttempts;
        m_notificationBaselineRetryTimer.start(
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::seconds{1U << exponent})
                                 .count()));
    }

    void AccountSyncCoordinator::scheduleNotificationBaselineRefresh()
    {
        if (!m_pendingNotificationBaselineMailboxIds.has_value() || !hasValidSettings() ||
            m_runContext == nullptr)
            return;

        const MailRefreshDemand demand{
            .mailboxState = false, .emailState = true, .allMailboxes = false, .mailboxIds = {}};
        if (m_refreshInFlight)
        {
            m_queuedRefreshDemand.merge(demand);
            return;
        }
        m_debouncedRefreshDemand.merge(demand);
        m_refreshDebounceTimer.start(0);
    }

    void AccountSyncCoordinator::scheduleCatchUpRefresh()
    {
        processGroupwareStateChanges();
        auto demand = std::exchange(m_debouncedRefreshDemand, {});
        if (m_pendingStateChanges.empty() && demand.empty())
            return;

        if (!m_pendingStateChanges.empty() && pendingStateChangesAlreadyApplied() && demand.empty())
        {
            m_pendingStateChanges.clear();
            return;
        }

        if (const auto mailbox = m_pendingStateChanges.find("Mailbox");
            mailbox != m_pendingStateChanges.end() &&
            !pendingStateChangeAlreadyApplied(mailbox->first, mailbox->second))
        {
            demand.mailboxState = true;
        }
        if (const auto email = m_pendingStateChanges.find("Email");
            email != m_pendingStateChanges.end() &&
            !pendingStateChangeAlreadyApplied(email->first, email->second))
        {
            demand.emailState = true;
        }
        m_pendingStateChanges.clear();
        if (demand.empty())
            return;

        if (m_refreshInFlight)
        {
            m_queuedRefreshDemand.merge(demand);
            return;
        }

        if (m_runContext == nullptr)
            return;
        auto retryLease = m_endpointRetryGate.acquire(m_runContext->configuration.apiUrl);
        if (!retryLease.allowed())
        {
            m_debouncedRefreshDemand.merge(demand);
            m_refreshDebounceTimer.start(
                static_cast<int>(std::max<std::int64_t>(1, retryLease.retryAfter().count())));
            return;
        }

        auto task = refreshWatchedMailbox(demand, std::move(retryLease));
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
                    if (shouldDeferForActiveMutation(type) &&
                        domainHasActiveMutation(accountId, type))
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
        process(m_pendingIdentityStateChanges, [this, &ownerAccountId](const auto& states)
                { Q_EMIT identityStateChanged(ownerAccountId, states); });
        if (!m_pendingCalendarStateChanges.empty() || !m_pendingContactStateChanges.empty() ||
            !m_pendingIdentityStateChanges.empty())
            m_refreshDebounceTimer.start(static_cast<int>(refreshDebounceInterval.count()));
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
        if (!hasValidSettings() || m_status == Status::AuthenticationPaused)
        {
            return;
        }

        m_methodTransport.invalidateConnection(m_accountId);
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
            m_runContext->configuration.remoteAccountId == nextConfiguration->remoteAccountId &&
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
                    m_stateChangeAuthenticationRetryToken.reset();
                    return;
                }

                setStatus(toServiceStatus(status));
            },
            [this, generation = runContext->generation](const javelin::jmap::OperationError& error)
            {
                if (m_runContext == nullptr || m_runContext->generation != generation)
                    return;
                handleStateChangeAuthenticationError(QStringLiteral("Maintain account connection"),
                                                     error);
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
            Q_EMIT stateChangeCatchUpRequired();
            scheduleDebouncedRefresh(true);
        }
        Q_EMIT statusChanged(m_status);
    }

    void AccountSyncCoordinator::handleStateChangeAuthenticationError(
        const QString& operation, const javelin::jmap::OperationError& error)
    {
        if (!javelin::jmap::isAuthenticationError(error) || !m_settings.has_value() ||
            !m_authenticationRefreshHandler)
        {
            publishOperationError(operation, error);
            return;
        }

        const auto rejectedAccessToken = m_settings->apiKey;
        if (m_stateChangeAuthenticationRetryToken == rejectedAccessToken)
        {
            publishOperationError(operation, error);
            return;
        }
        if (m_authenticationRecoveryInFlight)
            return;

        m_authenticationRecoveryInFlight = true;
        auto task = recoverStateChangeAuthentication(operation, error, m_generation, m_accountId,
                                                     rejectedAccessToken);
        QCoro::connect(std::move(task), this, []() {});
    }

    QCoro::Task<void> AccountSyncCoordinator::recoverStateChangeAuthentication(
        QString operation, javelin::jmap::OperationError error, const std::size_t generation,
        std::string accountId, std::string rejectedAccessToken)
    {
        auto refreshedAccessToken =
            co_await m_authenticationRefreshHandler(std::move(accountId), rejectedAccessToken);
        m_authenticationRecoveryInFlight = false;
        if (refreshedAccessToken.has_value() && *refreshedAccessToken != rejectedAccessToken)
        {
            m_stateChangeAuthenticationRetryToken = std::move(*refreshedAccessToken);
            co_return;
        }
        if (m_runContext == nullptr || m_runContext->generation != generation)
            co_return;

        publishOperationError(operation, error);
    }

    void AccountSyncCoordinator::publishOperationError(const QString& operation,
                                                       const javelin::jmap::OperationError& error)
    {
        Q_EMIT operationFailed(operation, error);
        if (javelin::jmap::isAuthenticationError(error))
            pauseForAuthentication();
    }

    void AccountSyncCoordinator::recordRefreshFailure(EndpointRetryGate::Lease& retryLease,
                                                      const javelin::jmap::OperationError& error)
    {
        if (usesEndpointBackoff(error))
        {
            retryLease.recordFailure(error.retryAfter);
            return;
        }

        if (error.code == javelin::jmap::OperationErrorCode::LocalStorageBusy ||
            error.code == javelin::jmap::OperationErrorCode::LocalStorageFailure)
        {
            return;
        }

        // A protocol, authentication, or method-level rejection proves that the endpoint answered.
        // Clear any stale transport outage before surfacing the independent operation error.
        retryLease.recordSuccess();
    }

    void AccountSyncCoordinator::recordRefreshSuccess(EndpointRetryGate::Lease& retryLease)
    {
        retryLease.recordSuccess();
        Q_EMIT operationSucceeded();
    }

    bool AccountSyncCoordinator::usesEndpointBackoff(const javelin::jmap::OperationError& error)
    {
        switch (error.code)
        {
        case javelin::jmap::OperationErrorCode::NetworkUnavailable:
        case javelin::jmap::OperationErrorCode::Timeout:
        case javelin::jmap::OperationErrorCode::RateLimited:
        case javelin::jmap::OperationErrorCode::ServerUnavailable:
            return true;
        default:
            return false;
        }
    }

} // namespace javelin::app
