#include "app/LongPollCoordinator.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/WorkScheduler.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <QDebug>
#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace javelin::app
{
    namespace
    {
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
    } // namespace

    namespace
    {
        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(const AccountConnectionSettings& settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = settings.sessionUrl,
                .loginEmail = settings.loginEmail,
                .apiKey = settings.apiKey,
            };
        }

        template <typename Result>
        [[nodiscard]] Result observeResult(ApplicationErrorCoordinator& coordinator,
                                           const AccountConnectionSettings& settings,
                                           const std::string_view accountId, QString operation,
                                           Result result)
        {
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                coordinator.reportFailure(settings, accountId, std::move(operation), *error);
            else
                coordinator.reportSuccess(settings.connectionId);
            return result;
        }
    } // namespace

    MailboxObservation::MailboxObservation(
        MailApplicationService& service,
        const javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId)
        : m_service(&service), m_observationId(observationId)
    {
    }

    MailboxObservation::~MailboxObservation()
    {
        reset();
    }

    MailboxObservation::MailboxObservation(MailboxObservation&& other) noexcept
        : m_service(std::exchange(other.m_service, nullptr)),
          m_observationId(std::exchange(other.m_observationId, 0))
    {
    }

    MailboxObservation& MailboxObservation::operator=(MailboxObservation&& other) noexcept
    {
        if (this == &other)
        {
            return *this;
        }

        reset();
        m_service = std::exchange(other.m_service, nullptr);
        m_observationId = std::exchange(other.m_observationId, 0);
        return *this;
    }

    void MailboxObservation::reset()
    {
        if (m_service != nullptr && m_observationId != 0)
        {
            m_service->releaseMailboxObservation(m_observationId);
        }
        m_service.clear();
        m_observationId = 0;
    }

    MailboxObservation::operator bool() const
    {
        return m_service != nullptr && m_observationId != 0;
    }

    MailApplicationService::MailApplicationService(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::JmapCore& jmapCore, javelin::jmap::api::JmapMethodTransport& methodTransport,
        QNetworkAccessManager& networkAccessManager,
        javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::QueryService& queryService,
        javelin::jmap::contacts::ContactService& contactService,
        javelin::jmap::calendar::CalendarService& calendarService,
        javelin::jmap::sieve::SieveService& sieveService,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
        QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_jmapCore(jmapCore),
          m_methodTransport(methodTransport), m_networkAccessManager(networkAccessManager),
          m_transportCooldowns(cooldowns), m_accountRepository(accountRepository),
          m_queryService(queryService), m_contactService(contactService),
          m_calendarService(calendarService), m_sieveService(sieveService),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler)
    {
        connect(&m_errorCoordinator, &ApplicationErrorCoordinator::authenticationPauseChanged, this,
                [this](const QString& connectionId, const bool paused)
                {
                    for (const auto& [accountId, configuration] : m_configurations)
                    {
                        if (configuration.settings.connectionId != connectionId.toStdString())
                            continue;
                        if (paused)
                        {
                            const auto coordinator = m_coordinators.find(accountId);
                            if (coordinator != m_coordinators.end())
                                coordinator->second->pauseForAuthentication();
                        }
                        else
                        {
                            applyAccountConfiguration(accountId);
                        }
                    }
                });
    }

    void MailApplicationService::applySettings(std::vector<AccountSyncConfiguration> configurations)
    {
        std::unordered_set<std::string> previousConnectionIds;
        for (const auto& [accountId, configuration] : m_configurations)
        {
            static_cast<void>(accountId);
            previousConnectionIds.insert(configuration.settings.connectionId);
        }
        std::unordered_set<std::string> configuredAccountIds;
        std::unordered_set<std::string> configuredConnectionIds;
        for (auto& configuration : configurations)
        {
            configuredConnectionIds.insert(configuration.settings.connectionId);
            const auto connectionId = configuration.settings.connectionId;
            const auto revision = configuration.settings.revision;
            configuredAccountIds.insert(configuration.accountId);
            const auto accountId = configuration.accountId;
            m_configurations.insert_or_assign(accountId, std::move(configuration));
            m_errorCoordinator.settingsApplied(connectionId, revision);
            applyAccountConfiguration(accountId);
        }

        for (auto coordinatorIt = m_coordinators.begin(); coordinatorIt != m_coordinators.end();)
        {
            if (configuredAccountIds.contains(coordinatorIt->first))
            {
                ++coordinatorIt;
                continue;
            }

            Q_EMIT accountStatusChanged(QString::fromStdString(coordinatorIt->first),
                                        AccountSyncCoordinator::Status::Disconnected);
            disconnect(coordinatorIt->second.get(), nullptr, this, nullptr);
            coordinatorIt = m_coordinators.erase(coordinatorIt);
        }
        std::erase_if(m_configurations, [&configuredAccountIds](const auto& entry)
                      { return !configuredAccountIds.contains(entry.first); });
        m_mailboxInterests.eraseAccountsNotIn(configuredAccountIds);
        for (const auto& connectionId : previousConnectionIds)
        {
            if (!configuredConnectionIds.contains(connectionId))
                m_errorCoordinator.forgetConnection(connectionId);
        }
        refreshConfiguredSessions();
    }

    void MailApplicationService::refreshConfiguredSessions()
    {
        javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
        for (const auto& [accountId, configuration] : m_configurations)
        {
            if (m_errorCoordinator.authenticationPaused(configuration.settings.connectionId,
                                                        configuration.settings.revision))
                continue;
            const auto loaded = sessions.load(accountId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&loaded))
            {
                qWarning().noquote() << "JMAP startup session lookup failed"
                                     << QString::fromStdString(accountId) << error->message;
                continue;
            }

            const auto& session = std::get<std::optional<javelin::jmap::api::Session>>(loaded);
            if (session.has_value())
            {
                startSessionRefresh(accountId, configuration.settings);
            }
        }
    }

    void MailApplicationService::startSessionRefresh(const std::string& ownerAccountId,
                                                     const AccountConnectionSettings& settings)
    {
        if (!m_sessionRefreshesInFlight.insert(ownerAccountId).second)
        {
            return;
        }

        m_workScheduler.beginForegroundWork();
        const auto appliedSettings = settings;
        auto task = m_jmapCore.refreshSession(toLiveConnectionSettings(settings), ownerAccountId);
        QCoro::connect(
            std::move(task), this,
            [this, ownerAccountId, appliedSettings](javelin::jmap::SessionRefreshResult result)
            {
                m_sessionRefreshesInFlight.erase(ownerAccountId);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote()
                        << "JMAP startup session discovery failed"
                        << QString::fromStdString(ownerAccountId) << error->message;
                    m_errorCoordinator.reportFailure(appliedSettings, ownerAccountId,
                                                     QStringLiteral("Discover server session"),
                                                     *error);
                    m_workScheduler.endForegroundWork();
                    return;
                }

                m_errorCoordinator.reportSuccess(appliedSettings.connectionId);

                const auto& summary = std::get<javelin::jmap::SessionRefreshSummary>(result);
                qInfo().noquote() << "JMAP startup session discovered"
                                  << QString::fromStdString(ownerAccountId)
                                  << (summary.websocketAdvertised ? QStringLiteral("WebSocket")
                                                                  : QStringLiteral("HTTP only"));
                for (const auto& [accountId, configuration] : m_configurations)
                {
                    if (configuration.settings.connectionId == appliedSettings.connectionId)
                    {
                        applyAccountConfiguration(accountId);
                    }
                }
                Q_EMIT sessionCapabilitiesChanged(QString::fromStdString(ownerAccountId));
                m_workScheduler.endForegroundWork();
            });
    }

    void MailApplicationService::applyAccountConfiguration(const std::string& accountId)
    {
        const auto stored = m_configurations.find(accountId);
        if (stored == m_configurations.end())
            return;

        auto configuration = stored->second;
        auto observedMailboxIds = m_mailboxInterests.mailboxIds(accountId);
        configuration.mailboxIds.insert(configuration.mailboxIds.end(),
                                        std::make_move_iterator(observedMailboxIds.begin()),
                                        std::make_move_iterator(observedMailboxIds.end()));
        std::ranges::sort(configuration.mailboxIds);
        configuration.mailboxIds.erase(std::ranges::unique(configuration.mailboxIds).begin(),
                                       configuration.mailboxIds.end());

        auto [coordinatorIt, inserted] = m_coordinators.try_emplace(accountId);
        if (inserted)
        {
            coordinatorIt->second = std::make_unique<AccountSyncCoordinator>(
                m_databaseConnection, m_methodTransport, m_networkAccessManager,
                m_transportCooldowns, m_accountRepository, m_queryService, m_workScheduler, this);
            connectCoordinator(coordinatorIt->first, *coordinatorIt->second);
        }
        if (m_errorCoordinator.authenticationPaused(configuration.settings.connectionId,
                                                    configuration.settings.revision))
        {
            coordinatorIt->second->pauseForAuthentication();
            return;
        }
        coordinatorIt->second->applySettings(std::move(configuration.settings), accountId,
                                             std::move(configuration.mailboxIds),
                                             std::move(configuration.notificationMailboxIds),
                                             configuration.notificationMailboxSelectionConfigured);
    }

    MailboxObservation MailApplicationService::observeMailbox(std::string accountId,
                                                              std::string mailboxId)
    {
        const auto configuredAccountId = accountId;
        const auto observationId =
            m_mailboxInterests.observe(std::move(accountId), std::move(mailboxId));
        applyAccountConfiguration(configuredAccountId);
        return MailboxObservation{*this, observationId};
    }

    void MailApplicationService::releaseMailboxObservation(
        const javelin::jmap::sync::MailboxInterestRegistry::ObservationId observationId)
    {
        const auto interest = m_mailboxInterests.unobserve(observationId);
        if (!interest.has_value())
        {
            return;
        }
        applyAccountConfiguration(interest->accountId);
    }

    bool MailApplicationService::requestAccountSynchronization(const std::string_view accountId)
    {
        const auto coordinator = m_coordinators.find(std::string{accountId});
        if (coordinator == m_coordinators.end())
            return false;
        return coordinator->second->requestSynchronization();
    }

    QString MailApplicationService::statusSummary() const
    {
        return m_jmapCore.statusSummary();
    }

    void MailApplicationService::publishMailboxWindowCommitted(QString accountId, QString mailboxId,
                                                               const std::size_t offset,
                                                               const std::size_t limit)
    {
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = std::move(accountId),
            .mailboxIds = {},
            .queryWindows = {MailboxQueryWindowChange{
                .mailboxId = std::move(mailboxId),
                .offset = offset,
                .limit = limit,
                .total = std::nullopt,
            }},
            .searchWindows = {},
            .mailboxTreeChanged = false,
            .hasNewMail = false,
        });
    }

    QCoro::Task<MailboxWindowResult>
    MailApplicationService::requestMailboxWindow(MailboxWindowIntent intent)
    {
        const auto configuration = m_configurations.find(intent.accountId);
        if (configuration == m_configurations.end())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        }

        const auto queryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = intent.mailboxId,
            .sortProperty = javelin::jmap::query::propertyName(intent.sort.property),
            .isAscending = javelin::jmap::query::isAscending(intent.sort),
            .collapseThreads = true,
        });
        const auto canonicalQueryKey = javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = intent.mailboxId,
            .sortProperty = "receivedAt",
            .isAscending = false,
            .collapseThreads = true,
        });
        const auto offlineStateResult = m_queryService.completeOfflineMailboxQueryState(
            intent.accountId, intent.mailboxId, canonicalQueryKey);
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&offlineStateResult))
        {
            co_return javelin::jmap::operationError(*error);
        }
        const auto& offlineState = std::get<std::optional<std::string>>(offlineStateResult);
        if (!intent.forceRefresh)
        {
            const auto cachedResult = m_queryService.loadMailboxWindow(
                intent.accountId, queryKey, intent.offset, intent.limit, intent.sort);
            if (const auto* cached =
                    std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
                        &cachedResult);
                cached != nullptr && cached->has_value() && (*cached)->isAuthoritative &&
                (!offlineState.has_value() || (*cached)->queryState == *offlineState))
            {
                co_return MailboxWindowSummary{
                    .accountId = std::move(intent.accountId),
                    .mailboxId = std::move(intent.mailboxId),
                    .offset = intent.offset,
                    .limit = intent.limit,
                    .position = (*cached)->position,
                    .returnedLimit = (*cached)->returnedLimit,
                    .representativeCount = (*cached)->items.size(),
                    .total = (*cached)->total,
                    .queryState = (*cached)->queryState,
                };
            }
        }
        if (offlineState.has_value())
        {
            const std::string& state = *offlineState;
            javelin::jmap::cache::MailboxWindowRepository windows{m_databaseConnection};
            if (!intent.forceRefresh)
            {
                const auto cachedResult =
                    windows.find(intent.accountId, queryKey, intent.offset, intent.limit);
                if (const auto* error =
                        std::get_if<javelin::jmap::cache::DatabaseError>(&cachedResult))
                    co_return javelin::jmap::operationError(*error);
                const auto& cached =
                    std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(
                        cachedResult);
                if (cached.has_value() && cached->isAuthoritative && cached->queryState == state)
                {
                    co_return MailboxWindowSummary{
                        .accountId = std::move(intent.accountId),
                        .mailboxId = std::move(intent.mailboxId),
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .position = cached->position,
                        .returnedLimit = cached->returnedLimit,
                        .representativeCount = cached->emailIds.size(),
                        .total = cached->total,
                        .queryState = cached->queryState,
                    };
                }
            }
            const auto itemsResult = m_queryService.listMailboxMessages(
                intent.accountId, intent.mailboxId, intent.limit, intent.offset, intent.sort);
            const auto totalResult =
                m_queryService.countMailboxMessages(intent.accountId, intent.mailboxId);
            const auto* items =
                std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&itemsResult);
            const auto* total = std::get_if<std::size_t>(&totalResult);
            if (items != nullptr && total != nullptr)
            {
                std::vector<std::string> emailIds;
                emailIds.reserve(items->size());
                for (const auto& item : *items)
                    emailIds.push_back(item.emailId);
                if (const auto error = windows.replace({
                        .accountId = intent.accountId,
                        .mailboxId = intent.mailboxId,
                        .queryKey = queryKey,
                        .requestedOffset = intent.offset,
                        .requestedLimit = intent.limit,
                        .position = intent.offset,
                        .returnedLimit = intent.limit,
                        .total = *total,
                        .queryState = state,
                        .isAuthoritative = true,
                        .emailIds = std::move(emailIds),
                    }))
                {
                    co_return javelin::jmap::operationError(*error);
                }
                co_return MailboxWindowSummary{
                    .accountId = std::move(intent.accountId),
                    .mailboxId = std::move(intent.mailboxId),
                    .offset = intent.offset,
                    .limit = intent.limit,
                    .position = intent.offset,
                    .returnedLimit = intent.limit,
                    .representativeCount = items->size(),
                    .total = *total,
                    .queryState = state,
                };
            }
        }

        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = co_await m_jmapCore.queryMailboxPage(
            toLiveConnectionSettings(configuration->second.settings), intent.accountId,
            intent.mailboxId, intent.offset, intent.limit, intent.sort, std::move(intent.anchor),
            intent.anchorOffset);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(configuration->second.settings, intent.accountId,
                                             QStringLiteral("Load mailbox messages"), *error);
            co_return *error;
        }
        m_errorCoordinator.reportSuccess(configuration->second.settings.connectionId);

        auto page = std::get<javelin::jmap::MailboxPageSummary>(std::move(result));
        MailboxWindowSummary summary{
            .accountId = page.accountId,
            .mailboxId = page.mailboxId,
            .offset = page.offset,
            .limit = page.limit,
            .position = page.position,
            .returnedLimit = page.returnedLimit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .queryState = page.queryState,
        };
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(page.accountId),
            .mailboxIds = {QString::fromStdString(page.mailboxId)},
            .queryWindows = {MailboxQueryWindowChange{
                .mailboxId = QString::fromStdString(page.mailboxId),
                .offset = page.offset,
                .limit = page.limit,
                .total = page.total,
            }},
            .searchWindows = {},
            .hasNewMail = false,
        });
        co_return summary;
    }

    QCoro::Task<SearchWindowResult>
    MailApplicationService::requestSearchWindow(SearchWindowIntent intent)
    {
        const auto configuration = m_configurations.find(intent.accountId);
        if (configuration == m_configurations.end())
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        }

        const auto queryKey = intent.windowKey.empty()
                                  ? javelin::jmap::search::cacheKey(intent.criteria, intent.sort)
                                  : intent.windowKey;
        if (m_retiredSearchWindowKeys.contains(queryKey))
        {
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("The search tab has been closed."),
            };
        }
        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = co_await m_jmapCore.searchMessages(
            toLiveConnectionSettings(configuration->second.settings), intent.accountId,
            intent.criteria, intent.offset, intent.limit, intent.sort, std::move(intent.anchor),
            queryKey);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(configuration->second.settings, intent.accountId,
                                             QStringLiteral("Search messages"), *error);
            co_return *error;
        }
        m_errorCoordinator.reportSuccess(configuration->second.settings.connectionId);

        const auto& page = std::get<javelin::jmap::MessageSearchSummary>(result);
        if (m_retiredSearchWindowKeys.contains(queryKey))
        {
            static_cast<void>(m_queryService.eraseSearchWindows(intent.accountId, queryKey));
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("The search tab has been closed."),
            };
        }
        SearchWindowSummary summary{
            .accountId = page.accountId,
            .queryKey = queryKey,
            .offset = page.offset,
            .limit = page.limit,
            .position = page.position,
            .returnedLimit = page.returnedLimit,
            .representativeCount = page.representativeCount,
            .total = page.total,
            .queryState = page.queryState,
        };
        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(page.accountId),
            .mailboxIds = {},
            .queryWindows = {},
            .searchWindows = {SearchQueryWindowChange{
                .queryKey = QString::fromStdString(queryKey),
                .offset = page.offset,
                .limit = page.limit,
                .total = page.total,
            }},
            .hasNewMail = false,
        });
        co_return summary;
    }

    void MailApplicationService::retireSearchWindow(std::string accountId, std::string windowKey)
    {
        m_retiredSearchWindowKeys.insert(windowKey);
        static_cast<void>(m_queryService.eraseSearchWindows(accountId, windowKey));
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueDestroyEmail(std::string accountId, std::string emailId)
    {
        return m_jmapCore.queueDestroyEmail(std::move(accountId), std::move(emailId));
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueMoveEmail(std::string accountId, std::string emailId,
                                           std::string sourceMailboxId,
                                           std::string destinationMailboxId)
    {
        return m_jmapCore.queueMoveEmail(std::move(accountId), std::move(emailId),
                                         std::move(sourceMailboxId),
                                         std::move(destinationMailboxId));
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueCopyEmail(std::string accountId, std::string emailId,
                                           std::string sourceMailboxId,
                                           std::string destinationMailboxId)
    {
        return m_jmapCore.queueCopyEmail(std::move(accountId), std::move(emailId),
                                         std::move(sourceMailboxId),
                                         std::move(destinationMailboxId));
    }

    QueuedMailboxSelectionMutationResult
    MailApplicationService::queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent)
    {
        const auto mailboxesResult = m_queryService.listMailboxTree(intent.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
        {
            return javelin::jmap::OperationError{.message = error->message};
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> emails;
        emails.reserve(intent.emailIds.size());
        std::unordered_set<std::string_view> seenEmailIds;
        for (const auto& emailId : intent.emailIds)
        {
            if (!seenEmailIds.insert(emailId).second)
            {
                continue;
            }
            const auto emailResult = emailRepository.find(intent.accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return javelin::jmap::OperationError{.message = error->message};
            }
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return javelin::jmap::OperationError{
                    .message = QStringLiteral("Message %1 is not available in the local cache.")
                                   .arg(QString::fromStdString(emailId)),
                };
            }
            emails.push_back(*email);
        }

        auto planResult = planMailboxSelectionMutation(
            intent, emails,
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult));
        if (const auto* error = std::get_if<QString>(&planResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }

        auto plan = std::get<PlannedMailboxSelectionMutation>(std::move(planResult));
        const auto queuedEmailCount = plan.mutations.size();
        for (auto& mutation : plan.mutations)
        {
            const auto result =
                m_jmapCore.queueEmailMailboxMutation(intent.accountId, std::move(mutation));
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            {
                return *error;
            }
        }

        return QueuedMailboxSelectionMutation{
            .accountId = std::move(intent.accountId),
            .queuedEmailCount = queuedEmailCount,
            .skippedEmailCount = plan.skippedEmailCount,
        };
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        return m_jmapCore.queueMarkEmailRead(std::move(accountId), std::move(emailId));
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueMarkEmailUnread(std::string accountId, std::string emailId)
    {
        return m_jmapCore.queueMarkEmailUnread(std::move(accountId), std::move(emailId));
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueSetEmailFlagged(std::string accountId, std::string emailId,
                                                 const bool flagged)
    {
        return m_jmapCore.queueSetEmailFlagged(std::move(accountId), std::move(emailId), flagged);
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailApplicationService::submitPendingEmailMutations(std::string accountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, accountId,
            QStringLiteral("Submit pending mail changes"),
            co_await m_jmapCore.submitPendingEmailMutations(
                toLiveConnectionSettings(configuration->second.settings), accountId));
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    MailApplicationService::requestMessageContent(std::string accountId, std::string emailId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        const ForegroundWorkScope foreground{m_workScheduler};
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                QStringLiteral("Load message content"),
                                co_await m_jmapCore.refreshMessageContent(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    accountId, std::move(emailId)));
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    MailApplicationService::requestAttachment(std::string accountId, std::string emailId,
                                              std::string partId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                QStringLiteral("Download attachment"),
                                co_await m_jmapCore.downloadAttachment(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    accountId, std::move(emailId), std::move(partId)));
    }

    QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
    MailApplicationService::requestMessageSource(std::string accountId, std::string emailId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        co_return co_await m_jmapCore.loadCachedMessageSource(std::move(accountId),
                                                              std::move(emailId));
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    MailApplicationService::bootstrapAccount(AccountBootstrapIntent intent)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = co_await m_jmapCore.refreshFromServer(
            toLiveConnectionSettings(intent.settings), {}, std::move(intent.mailboxIds));
        co_return observeResult(m_errorCoordinator, intent.settings, {},
                                QStringLiteral("Synchronize account"), std::move(result));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    MailApplicationService::requestContacts(std::string accountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, accountId,
            QStringLiteral("Synchronize contacts"),
            co_await m_contactService.refreshAll(
                toLiveConnectionSettings(configuration->second.settings), accountId));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    MailApplicationService::requestCalendarRange(
        std::string ownerAccountId, javelin::jmap::calendar::VisibleInterval interval,
        javelin::jmap::calendar::TimeZoneId displayTimeZone)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        m_visibleCalendarRanges.insert_or_assign(
            ownerAccountId,
            VisibleCalendarRange{.interval = interval, .displayTimeZone = displayTimeZone});
        auto result = co_await m_calendarService.refresh(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId, interval,
            displayTimeZone);
        if (const auto* summary = std::get_if<javelin::jmap::calendar::RefreshedRange>(&result);
            summary != nullptr && summary->accountCount > 0)
        {
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = summary->interval,
                                           .displayTimeZone = summary->displayTimeZone,
                                           .accountCount = summary->accountCount,
                                           .eventCount = summary->eventCount});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Synchronize calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
    MailApplicationService::requestCalendarChanges(std::string ownerAccountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        const auto range = m_visibleCalendarRanges.find(ownerAccountId);
        if (configuration == m_configurations.end() || range == m_visibleCalendarRanges.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Calendar synchronization is not configured.")};
        auto result = co_await m_calendarService.refreshChanged(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            range->second.interval, range->second.displayTimeZone);
        if (const auto* summary = std::get_if<javelin::jmap::calendar::RefreshedRange>(&result);
            summary != nullptr && summary->accountCount > 0)
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = summary->interval,
                                           .displayTimeZone = summary->displayTimeZone,
                                           .accountCount = summary->accountCount,
                                           .eventCount = summary->eventCount});
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Synchronize calendar changes"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::createCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::CreateEventCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Create calendar event"),
                                co_await m_calendarService.create(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(command), projectionCommitted));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                                               std::string calendarId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        auto result = co_await m_calendarService.setDefaultCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(accountId), std::move(calendarId));
        if (std::holds_alternative<javelin::jmap::calendar::CommittedMutation>(result))
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range != m_visibleCalendarRanges.end())
                Q_EMIT calendarCacheCommitted(
                    {.ownerAccountId = QString::fromStdString(ownerAccountId),
                     .interval = range->second.interval,
                     .displayTimeZone = range->second.displayTimeZone,
                     .accountCount = 1,
                     .eventCount = 0});
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Change default calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::updateCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::UpdateEventCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Update calendar event"),
                                co_await m_calendarService.update(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(command), projectionCommitted));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::deleteCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::DeleteEventCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        const auto projectionCommitted = [this, ownerAccountId]
        {
            const auto range = m_visibleCalendarRanges.find(ownerAccountId);
            if (range == m_visibleCalendarRanges.end())
                return;
            Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                           .interval = range->second.interval,
                                           .displayTimeZone = range->second.displayTimeZone,
                                           .accountCount = 1,
                                           .eventCount = 0});
        };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Delete calendar event"),
                                co_await m_calendarService.remove(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(command), projectionCommitted));
    }

    QCoro::Task<javelin::jmap::sieve::SieveListResult>
    MailApplicationService::requestSieveScripts(std::string ownerAccountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            QStringLiteral("Load Sieve scripts"),
            co_await m_sieveService.list(toLiveConnectionSettings(configuration->second.settings),
                                         ownerAccountId));
    }

    QCoro::Task<javelin::jmap::sieve::SieveContentResult>
    MailApplicationService::requestSieveScript(std::string ownerAccountId,
                                               javelin::jmap::sieve::SieveScript script)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            QStringLiteral("Load Sieve script"),
            co_await m_sieveService.load(toLiveConnectionSettings(configuration->second.settings),
                                         ownerAccountId, std::move(script)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveValidationResult>
    MailApplicationService::validateSieveScript(std::string ownerAccountId, QByteArray content)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Validate Sieve script"),
                                co_await m_sieveService.validate(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(content)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveSaveResult> MailApplicationService::saveSieveScript(
        std::string ownerAccountId, javelin::jmap::sieve::SieveScript script, QByteArray content)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            QStringLiteral("Save Sieve script"),
            co_await m_sieveService.save(toLiveConnectionSettings(configuration->second.settings),
                                         ownerAccountId, std::move(script), std::move(content)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
    MailApplicationService::deleteSieveScript(std::string ownerAccountId,
                                              javelin::jmap::sieve::SieveScript script)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            QStringLiteral("Delete Sieve script"),
            co_await m_sieveService.remove(toLiveConnectionSettings(configuration->second.settings),
                                           ownerAccountId, std::move(script)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
    MailApplicationService::setSieveScriptActive(std::string ownerAccountId,
                                                 javelin::jmap::sieve::SieveScript script,
                                                 const bool active)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = QStringLiteral("Account synchronization is not configured.")};
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Change active Sieve script"),
                                co_await m_sieveService.setActive(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(script), active));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::setAddressBooks(std::string accountId,
                                            javelin::jmap::api::AddressBookSetRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                QStringLiteral("Change address books"),
                                co_await m_contactService.setAddressBooks(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    accountId, std::move(request)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::setContactCards(std::string accountId,
                                            javelin::jmap::api::ContactCardSetRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                QStringLiteral("Change contacts"),
                                co_await m_contactService.setContactCards(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    accountId, std::move(request)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::createContactGroup(
        std::string ownerAccountId, javelin::jmap::contacts::CreateContactGroupCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Create contact group"),
                                co_await m_contactService.createGroup(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(command)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::setContactGroupMembership(
        std::string ownerAccountId,
        javelin::jmap::contacts::SetContactGroupMembershipCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Change contact group membership"),
                                co_await m_contactService.setGroupMembership(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(command)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::copyContactCards(std::string accountId,
                                             javelin::jmap::api::ContactCardCopyRequest request)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                QStringLiteral("Copy contacts"),
                                co_await m_contactService.copyContactCards(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    accountId, std::move(request)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
    MailApplicationService::uploadContactMedia(std::string ownerAccountId, std::string accountId,
                                               QByteArray payload, std::string mediaType)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Upload contact media"),
                                co_await m_contactService.uploadMedia(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(accountId), std::move(payload),
                                    std::move(mediaType)));
    }

    QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
    MailApplicationService::downloadContactMedia(std::string ownerAccountId, std::string accountId,
                                                 std::string blobId, std::string mediaType)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::PreconditionFailed,
                .message = QStringLiteral("Account synchronization is not configured."),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                QStringLiteral("Download contact media"),
                                co_await m_contactService.downloadMedia(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(accountId), std::move(blobId),
                                    std::move(mediaType)));
    }

    void MailApplicationService::stop()
    {
        for (const auto& [accountId, coordinator] : m_coordinators)
        {
            Q_EMIT accountStatusChanged(QString::fromStdString(accountId),
                                        AccountSyncCoordinator::Status::Disconnected);
            disconnect(coordinator.get(), nullptr, this, nullptr);
        }
        m_coordinators.clear();
        m_configurations.clear();
        m_mailboxInterests.clear();
    }

    void MailApplicationService::connectCoordinator(const std::string& accountId,
                                                    AccountSyncCoordinator& coordinator)
    {
        connect(&coordinator, &AccountSyncCoordinator::statusChanged, this,
                [this, accountId](const auto status)
                { Q_EMIT accountStatusChanged(QString::fromStdString(accountId), status); });
        connect(&coordinator, &AccountSyncCoordinator::cacheCommitted, this,
                &MailApplicationService::cacheCommitted);
        connect(&coordinator, &AccountSyncCoordinator::calendarStateChanged, this,
                [this](const QString& ownerAccountId)
                {
                    const auto owner = ownerAccountId.toStdString();
                    const auto range = m_visibleCalendarRanges.find(owner);
                    if (range == m_visibleCalendarRanges.end())
                        return;
                    auto task = requestCalendarChanges(owner);
                    QCoro::connect(std::move(task), this,
                                   [](const javelin::jmap::calendar::CalendarRefreshResult& result)
                                   {
                                       if (const auto* error =
                                               std::get_if<javelin::jmap::OperationError>(&result))
                                           qWarning().noquote()
                                               << "Calendar state-change refresh failed"
                                               << error->message;
                                   });
                });
        connect(&coordinator, &AccountSyncCoordinator::notificationRaised, this,
                &MailApplicationService::notificationRaised);
        connect(
            &coordinator, &AccountSyncCoordinator::operationFailed, this,
            [this, accountId](const QString& operation, const javelin::jmap::OperationError& error)
            {
                const auto configuration = m_configurations.find(accountId);
                if (configuration != m_configurations.end())
                    m_errorCoordinator.reportFailure(configuration->second.settings, accountId,
                                                     operation, error);
            });
    }

} // namespace javelin::app
