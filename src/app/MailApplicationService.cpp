#include "app/MailApplicationService.h"

#include "app/ApplicationErrorCoordinator.h"
#include "app/MailboxMaintenanceRegistry.h"
#include "app/StateChangePolicy.h"
#include "app/WorkScheduler.h"
#include "app/undo/HistoryTypes.h"
#include "app/undo/UndoManager.h"

#include "jmap/OperationError.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QDebug>
#include <QNetworkAccessManager>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace javelin::app
{
    namespace
    {
        constexpr std::size_t pendingEmailMutationBatchSize = 25;

        [[nodiscard]] QString accountSynchronizationNotConfigured()
        {
            return i18n("Account synchronization is not configured.");
        }

        [[nodiscard]] std::variant<bool, javelin::jmap::cache::DatabaseError>
        emailMaintenanceActive(javelin::jmap::cache::DatabaseConnection& connection,
                               const MailboxMaintenanceRegistry& registry,
                               const std::string_view accountId, const std::string_view emailId)
        {
            javelin::jmap::cache::EmailRepository emails{connection};
            const auto found = emails.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                return *error;
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            if (!email.has_value())
                return false;
            QStringList mailboxIds;
            mailboxIds.reserve(static_cast<qsizetype>(email->mailboxIds.size()));
            for (const auto& mailboxId : email->mailboxIds)
                mailboxIds.push_back(QString::fromStdString(mailboxId));
            return registry.isActiveForEmail(QString::fromStdString(std::string{accountId}),
                                             mailboxIds);
        }

        [[nodiscard]] QStringList
        affectedMailboxIdsForPendingMutations(javelin::jmap::cache::DatabaseConnection& connection,
                                              const std::string_view accountId,
                                              const std::optional<std::string>& operationGroupId)
        {
            javelin::jmap::sync::EmailMutationJournal journal{connection};
            auto recordsResult =
                operationGroupId.has_value()
                    ? journal.listForOperationGroup(accountId, *operationGroupId)
                    : journal.listByStatus(accountId, javelin::jmap::sync::MutationStatus::Pending,
                                           pendingEmailMutationBatchSize);
            const auto* records =
                std::get_if<std::vector<javelin::jmap::sync::EmailMutationRecord>>(&recordsResult);
            if (records == nullptr)
                return {};

            QStringList mailboxIds;
            const auto append = [&mailboxIds](const std::vector<std::string>& values)
            {
                for (const auto& value : values)
                {
                    const auto mailboxId = QString::fromStdString(value);
                    if (!mailboxIds.contains(mailboxId))
                        mailboxIds.push_back(mailboxId);
                }
            };
            for (const auto& record : *records)
            {
                append(record.patch.addMailboxIds);
                append(record.patch.removeMailboxIds);
                if (record.baseMailboxIds.has_value())
                    append(*record.baseMailboxIds);
            }
            return mailboxIds;
        }

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

        constexpr auto contactRefreshRetryDelay = std::chrono::seconds{30};

        [[nodiscard]] javelin::app::undo::ExactMailPatch
        historyPatch(const javelin::jmap::EmailMailboxMutation& mutation)
        {
            return {
                .addMailboxIds = mutation.addMailboxIds,
                .removeMailboxIds = mutation.removeMailboxIds,
                .addKeywords = mutation.addKeywords,
                .removeKeywords = mutation.removeKeywords,
            };
        }

        [[nodiscard]] javelin::app::undo::ExactMailPatch
        inverseHistoryPatch(const javelin::jmap::EmailMailboxMutation& mutation)
        {
            return {
                .addMailboxIds = mutation.removeMailboxIds,
                .removeMailboxIds = mutation.addMailboxIds,
                .addKeywords = mutation.removeKeywords,
                .removeKeywords = mutation.addKeywords,
            };
        }

        [[nodiscard]] javelin::app::undo::MailPatchItemHistory
        historyItem(const std::string_view accountId, const javelin::jmap::domain::Email& email,
                    const javelin::jmap::EmailMailboxMutation& mutation)
        {
            return {
                .accountId = std::string{accountId},
                .emailId = email.id,
                .subject = email.subject,
                .forward = historyPatch(mutation),
                .inverse = inverseHistoryPatch(mutation),
                .expectedBefore = historyPatch(mutation),
                .expectedAfter = inverseHistoryPatch(mutation),
                .mutationId = std::nullopt,
            };
        }

        [[nodiscard]] QString messageCountLabel(const QString& verb, const std::size_t count)
        {
            return count == 1 ? verb + QStringLiteral(" Message")
                              : verb + QStringLiteral(" %1 Messages").arg(count);
        }

        [[nodiscard]] std::string contactRefreshJobId(const std::string_view ownerAccountId)
        {
            return "contact-refresh:" + std::string{ownerAccountId};
        }

        [[nodiscard]] std::string searchWindowLeaseKey(const std::string_view accountId,
                                                       const std::string_view queryKey)
        {
            std::string key;
            key.reserve(accountId.size() + queryKey.size() + 1);
            key.append(accountId);
            key.push_back('\0');
            key.append(queryKey);
            return key;
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

        [[nodiscard]] std::optional<javelin::jmap::OperationError>
        retainUnknownOrDiscard(javelin::app::undo::UndoManager& manager,
                               std::optional<javelin::app::undo::HistoryEntry>& prepared,
                               const javelin::jmap::OperationError& operationError)
        {
            if (!prepared.has_value())
                return std::nullopt;
            if (!javelin::jmap::isTransientError(operationError) ||
                javelin::jmap::isAuthenticationError(operationError))
            {
                if (const auto error = manager.discardNormal(prepared->entryId))
                    return javelin::jmap::operationError(*error);
                return std::nullopt;
            }
            auto committed = manager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                return javelin::jmap::operationError(*error);
            const auto& entry = std::get<javelin::app::undo::HistoryEntry>(committed);
            if (const auto error = manager.setEntryStatus(
                    entry.entryId, javelin::app::undo::HistoryEntryStatus::BlockedUnknown,
                    operationError.message))
                return javelin::jmap::operationError(*error);
            return std::nullopt;
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
        javelin::jmap::cache::ContactRepository& contactRepository,
        javelin::jmap::contacts::ContactService& contactService,
        javelin::jmap::calendar::CalendarService& calendarService,
        javelin::jmap::sieve::SieveService& sieveService,
        ApplicationErrorCoordinator& errorCoordinator, WorkScheduler& workScheduler,
        MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
        javelin::app::undo::UndoManager& undoManager, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_jmapCore(jmapCore),
          m_methodTransport(methodTransport), m_networkAccessManager(networkAccessManager),
          m_transportCooldowns(cooldowns), m_accountRepository(accountRepository),
          m_queryService(queryService), m_contactService(contactService),
          m_calendarService(calendarService), m_sieveService(sieveService),
          m_errorCoordinator(errorCoordinator), m_workScheduler(workScheduler),
          m_mailboxMaintenanceRegistry(mailboxMaintenanceRegistry), m_undoManager(undoManager)
    {
        connect(&contactRepository, &javelin::jmap::cache::ContactRepository::contactsChanged, this,
                [this](const QString& accountId)
                {
                    Q_EMIT cacheCommitted(MailCacheChange{
                        .accountId = accountId,
                        .mailboxIds = {},
                        .queryWindows = {},
                        .searchWindows = {},
                        .mailboxTreeChanged = false,
                        .hasNewMail = false,
                        .optimisticProjection = false,
                        .contactsChanged = true,
                    });
                });
        connect(&m_workScheduler, &WorkScheduler::jobsChanged, this,
                [this]() { scheduleContactRefreshPump(); });
        connect(&m_workScheduler, &WorkScheduler::foregroundAvailabilityChanged, this,
                [this]() { scheduleContactRefreshPump(); });
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
        const bool accountConfigurationsChanged =
            m_configurations.size() != configurations.size() ||
            std::ranges::any_of(configurations,
                                [this](const AccountSyncConfiguration& configuration)
                                {
                                    const auto previous =
                                        m_configurations.find(configuration.accountId);
                                    return previous == m_configurations.end() ||
                                           !(previous->second == configuration);
                                });
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
            const auto previous = m_configurations.find(accountId);
            const bool configurationChanged =
                previous == m_configurations.end() || previous->second != configuration;
            m_configurations.insert_or_assign(accountId, std::move(configuration));
            m_errorCoordinator.settingsApplied(connectionId, revision);
            if (configurationChanged)
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
        std::erase_if(m_pendingContactRefreshes,
                      [&configuredAccountIds](const std::string& accountId)
                      { return !configuredAccountIds.contains(accountId); });
        m_mailboxInterests.eraseAccountsNotIn(configuredAccountIds);
        for (const auto& connectionId : previousConnectionIds)
        {
            if (!configuredConnectionIds.contains(connectionId))
                m_errorCoordinator.forgetConnection(connectionId);
        }
        restoreContactRefreshJobs();
        if (accountConfigurationsChanged)
            refreshConfiguredSessions();
    }

    void MailApplicationService::setAuthenticationRefreshHandler(
        javelin::jmap::auth::AccessTokenRefreshHandler handler)
    {
        m_authenticationRefreshHandler = std::move(handler);
    }

    void MailApplicationService::networkBecameReachable()
    {
        m_networkAccessManager.clearConnectionCache();
        for (const auto& [accountId, coordinator] : m_coordinators)
        {
            coordinator->networkBecameReachable();
            schedulePendingEmailMutationReplay(accountId);
        }
    }

    std::unordered_map<std::string, AccountSyncCoordinator::Status>
    MailApplicationService::accountStatuses() const
    {
        std::unordered_map<std::string, AccountSyncCoordinator::Status> statuses;
        statuses.reserve(m_coordinators.size());
        for (const auto& [accountId, coordinator] : m_coordinators)
            statuses.emplace(accountId, coordinator->status());
        return statuses;
    }

    std::optional<AccountConnectionSettings>
    MailApplicationService::connectionSettingsFor(const std::string_view ownerAccountId) const
    {
        const auto configuration = m_configurations.find(std::string{ownerAccountId});
        return configuration != m_configurations.end()
                   ? std::optional{configuration->second.settings}
                   : std::nullopt;
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
                schedulePendingEmailMutationReplay(ownerAccountId);
            });
    }

    void MailApplicationService::schedulePendingEmailMutationReplay(std::string accountId)
    {
        if (!m_configurations.contains(accountId) ||
            !m_pendingMutationReplaysInFlight.insert(accountId).second)
        {
            return;
        }

        QTimer::singleShot(
            0, this,
            [this, accountId = std::move(accountId)]
            {
                auto task = submitPendingEmailMutations(accountId);
                QCoro::connect(
                    std::move(task), this,
                    [this, accountId](javelin::jmap::SubmittedEmailMutationsResult result)
                    {
                        m_pendingMutationReplaysInFlight.erase(accountId);
                        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        {
                            qWarning().noquote()
                                << "Queued mail replay failed" << QString::fromStdString(accountId)
                                << error->message;
                            return;
                        }

                        const auto& summary =
                            std::get<javelin::jmap::SubmittedEmailMutations>(result);
                        if (summary.attemptedEmailCount == 0)
                            return;
                        qInfo().noquote()
                            << "Queued mail replay submitted" << QString::fromStdString(accountId)
                            << summary.updatedEmailCount << "updated" << summary.failedEmailCount
                            << "failed";
                        if (m_configurations.contains(accountId))
                            schedulePendingEmailMutationReplay(accountId);
                    });
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
                m_transportCooldowns, m_accountRepository, m_queryService, m_workScheduler,
                m_authenticationRefreshHandler, this);
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
                                             std::move(configuration.notificationMailboxIds));
        if (m_pendingContactRefreshes.contains(accountId))
            scheduleContactRefresh(accountId);
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

    MailboxObservationLease MailApplicationService::beginMailboxObservation(std::string accountId,
                                                                            std::string mailboxId)
    {
        auto observation = std::make_shared<MailboxObservation>(
            observeMailbox(std::move(accountId), std::move(mailboxId)));
        return MailboxObservationLease{[observation = std::move(observation)]() mutable
                                       { observation.reset(); }};
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

    bool MailApplicationService::beginSearchWindowRequest(const std::string& leaseKey)
    {
        auto& state = m_searchWindowRequests[leaseKey];
        if (state.retired)
            return false;
        ++state.activeRequests;
        return true;
    }

    void MailApplicationService::finishSearchWindowRequest(const std::string& leaseKey)
    {
        const auto found = m_searchWindowRequests.find(leaseKey);
        if (found == m_searchWindowRequests.end())
            return;
        if (found->second.activeRequests > 0)
            --found->second.activeRequests;
        if (found->second.retired && found->second.activeRequests == 0)
            m_searchWindowRequests.erase(found);
    }

    bool MailApplicationService::searchWindowRetired(const std::string& leaseKey) const
    {
        const auto found = m_searchWindowRequests.find(leaseKey);
        return found != m_searchWindowRequests.end() && found->second.retired;
    }

    bool MailApplicationService::requestAccountSynchronization(const std::string_view accountId)
    {
        const auto coordinator = m_coordinators.find(std::string{accountId});
        if (coordinator == m_coordinators.end())
            return false;
        return coordinator->second->requestSynchronization();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailApplicationService::markMailNotificationsDelivered(const std::string_view accountId,
                                                           const std::string_view mailboxId,
                                                           const QStringList& emailIds)
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(emailIds.size()));
        for (const auto& emailId : emailIds)
            ids.push_back(emailId.toStdString());
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        return notifications.markDelivered(accountId, mailboxId, ids);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailApplicationService::releaseMailNotificationDispatches(const std::string_view accountId,
                                                              const QStringList& emailIds)
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(emailIds.size()));
        for (const auto& emailId : emailIds)
            ids.push_back(emailId.toStdString());
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        return notifications.releaseDispatches(accountId, ids);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    MailApplicationService::recoverMailNotificationDispatches()
    {
        javelin::jmap::cache::NotificationRepository notifications{m_databaseConnection};
        return notifications.recoverDispatches();
    }

    void MailApplicationService::publishCacheChange(MailCacheChange change)
    {
        Q_EMIT cacheCommitted(std::move(change));
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
        if (m_mailboxMaintenanceRegistry.isActive(QString::fromStdString(intent.accountId),
                                                  QString::fromStdString(intent.mailboxId)))
        {
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        }
        const auto configuration = m_configurations.find(intent.accountId);
        if (configuration == m_configurations.end())
        {
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
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
                cached != nullptr && cached->has_value())
            {
                const bool projectedDisplayCurrent =
                    (*cached)->coverage ==
                        javelin::jmap::cache::QueryWindowCoverage::LocallyProjected &&
                    javelin::jmap::cache::isDisplayCurrent((*cached)->coverage,
                                                           (*cached)->materialization);
                const bool authoritativeCurrent =
                    javelin::jmap::cache::isPaginationAuthoritative((*cached)->coverage,
                                                                    (*cached)->materialization) &&
                    (!offlineState.has_value() || (*cached)->queryState == *offlineState);
                if (projectedDisplayCurrent || authoritativeCurrent)
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
                if (cached.has_value() &&
                    javelin::jmap::cache::isPaginationAuthoritative(cached->coverage,
                                                                    cached->materialization) &&
                    cached->queryState == state)
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
                        .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
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
                .message = accountSynchronizationNotConfigured(),
            };
        }

        const auto queryKey = intent.windowKey.empty()
                                  ? javelin::jmap::search::cacheKey(intent.criteria, intent.sort)
                                  : intent.windowKey;
        const auto leaseKey = searchWindowLeaseKey(intent.accountId, queryKey);
        if (!beginSearchWindowRequest(leaseKey))
        {
            co_return javelin::jmap::OperationError{
                .message = i18n("The search tab has been closed."),
            };
        }
        const auto requestLease =
            qScopeGuard([this, leaseKey]() { finishSearchWindowRequest(leaseKey); });
        if (intent.criteria.inMailbox.has_value())
        {
            const auto& mailboxId = *intent.criteria.inMailbox;
            const auto canonicalQueryKey = javelin::jmap::sync::mailboxQueryKey({
                .mailboxId = mailboxId,
                .sortProperty = "receivedAt",
                .isAscending = false,
                .collapseThreads = true,
            });
            const auto offlineStateResult = m_queryService.completeOfflineMailboxQueryState(
                intent.accountId, mailboxId, canonicalQueryKey);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&offlineStateResult))
            {
                co_return javelin::jmap::operationError(*error);
            }
            const auto& offlineState = std::get<std::optional<std::string>>(offlineStateResult);
            if (offlineState.has_value())
            {
                const auto itemsResult = m_queryService.listFilteredMailboxMessages(
                    intent.accountId, mailboxId, intent.criteria, intent.limit, intent.offset,
                    intent.sort);
                const auto totalResult = m_queryService.countFilteredMailboxMessages(
                    intent.accountId, mailboxId, intent.criteria);
                const auto* items =
                    std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&itemsResult);
                const auto* total = std::get_if<std::size_t>(&totalResult);
                if (items == nullptr)
                    co_return javelin::jmap::operationError(
                        std::get<javelin::jmap::cache::DatabaseError>(itemsResult));
                if (total == nullptr)
                    co_return javelin::jmap::operationError(
                        std::get<javelin::jmap::cache::DatabaseError>(totalResult));

                std::vector<std::string> emailIds;
                emailIds.reserve(items->size());
                for (const auto& item : *items)
                    emailIds.push_back(item.emailId);
                javelin::jmap::cache::SearchWindowRepository searchWindows{m_databaseConnection};
                if (const auto error = searchWindows.replace({
                        .accountId = intent.accountId,
                        .queryKey = queryKey,
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .position = intent.offset,
                        .returnedLimit = intent.limit,
                        .total = *total,
                        .queryState = *offlineState,
                        .emailIds = emailIds,
                    }))
                {
                    co_return javelin::jmap::operationError(*error);
                }

                Q_EMIT cacheCommitted(MailCacheChange{
                    .accountId = QString::fromStdString(intent.accountId),
                    .mailboxIds = {},
                    .queryWindows = {},
                    .searchWindows = {SearchQueryWindowChange{
                        .queryKey = QString::fromStdString(queryKey),
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .total = *total,
                    }},
                    .hasNewMail = false,
                });
                co_return SearchWindowSummary{
                    .accountId = std::move(intent.accountId),
                    .queryKey = queryKey,
                    .offset = intent.offset,
                    .limit = intent.limit,
                    .position = intent.offset,
                    .returnedLimit = intent.limit,
                    .representativeCount = items->size(),
                    .total = *total,
                    .queryState = *offlineState,
                };
            }
        }

        javelin::jmap::search::EmailSearchResolution resolution;
        if (intent.criteria.fromContactsOnly)
        {
            const auto contacts = m_queryService.listContactEmailAddresses();
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&contacts))
                co_return javelin::jmap::operationError(*error);
            resolution.contactAddresses = std::get<std::vector<std::string>>(contacts);
        }
        if (intent.criteria.taggedOnly && intent.criteria.tags.empty())
        {
            const auto keywords = m_queryService.listUserKeywords(
                intent.accountId, intent.criteria.inMailbox.value_or(std::string{}));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&keywords))
                co_return javelin::jmap::operationError(*error);
            resolution.userKeywords = std::get<std::vector<std::string>>(keywords);
        }

        const auto settings = configuration->second.settings;
        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = co_await m_jmapCore.searchMessages(
            toLiveConnectionSettings(settings), intent.accountId, intent.criteria, intent.offset,
            intent.limit, intent.sort, std::move(intent.anchor), queryKey, {},
            std::move(resolution));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            m_errorCoordinator.reportFailure(settings, intent.accountId,
                                             QStringLiteral("Search messages"), *error);
            co_return *error;
        }
        m_errorCoordinator.reportSuccess(settings.connectionId);

        const auto& page = std::get<javelin::jmap::MessageSearchSummary>(result);
        if (searchWindowRetired(leaseKey))
        {
            static_cast<void>(m_queryService.eraseSearchWindows(intent.accountId, queryKey));
            co_return javelin::jmap::OperationError{
                .message = i18n("The search tab has been closed."),
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
        const auto leaseKey = searchWindowLeaseKey(accountId, windowKey);
        auto& state = m_searchWindowRequests[leaseKey];
        state.retired = true;
        static_cast<void>(m_queryService.eraseSearchWindows(accountId, windowKey));
        if (state.activeRequests == 0)
            m_searchWindowRequests.erase(leaseKey);
    }

    QueuedMailboxSelectionMutationResult
    MailApplicationService::queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent)
    {
        auto emailIdsResult = resolveMessageSelection(m_queryService, intent.accountId,
                                                      intent.sourceMailboxId, intent.selection);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }
        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));

        const auto mailboxesResult = m_queryService.listMailboxTree(intent.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxesResult))
        {
            return javelin::jmap::OperationError{.message = error->message};
        }

        javelin::jmap::cache::EmailRepository emailRepository{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> emails;
        emails.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            const auto emailResult = emailRepository.find(intent.accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&emailResult))
            {
                return javelin::jmap::OperationError{.message = error->message};
            }
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
            if (!email.has_value())
            {
                return javelin::jmap::OperationError{
                    .message = i18n("Message %1 is not available in the local cache.",
                                    QString::fromStdString(emailId)),
                };
            }
            emails.push_back(*email);
        }

        auto planResult = planMailboxSelectionMutation(
            intent, emailIds, emails,
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult));
        if (const auto* error = std::get_if<QString>(&planResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }

        auto plan = std::get<PlannedMailboxSelectionMutation>(std::move(planResult));
        const auto queuedEmailCount = plan.mutations.size();
        if (queuedEmailCount == 0)
        {
            return QueuedMailboxSelectionMutation{
                .accountId = std::move(intent.accountId),
                .queuedEmailCount = 0,
                .skippedEmailCount = plan.skippedEmailCount,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto& mailboxes =
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxesResult);
        const auto destination = [&mailboxes, &intent]
        {
            if (intent.destinationMailboxId.has_value())
            {
                return std::ranges::find(mailboxes, *intent.destinationMailboxId,
                                         &javelin::jmap::cache::MailboxTreeItem::id);
            }
            const auto role = intent.operation == MailboxSelectionOperation::Archive
                                  ? std::optional<std::string>{"archive"}
                              : intent.operation == MailboxSelectionOperation::Junk
                                  ? std::optional<std::string>{"junk"}
                              : intent.operation == MailboxSelectionOperation::NotJunk
                                  ? std::optional<std::string>{"inbox"}
                                  : std::optional<std::string>{std::nullopt};
            return std::ranges::find(mailboxes, role, &javelin::jmap::cache::MailboxTreeItem::role);
        }();
        QString label;
        if (intent.operation == MailboxSelectionOperation::Archive)
        {
            label = messageCountLabel(QStringLiteral("Archive"), queuedEmailCount);
        }
        else if (intent.operation == MailboxSelectionOperation::Junk)
        {
            label = messageCountLabel(QStringLiteral("Mark Junk"), queuedEmailCount);
        }
        else if (intent.operation == MailboxSelectionOperation::NotJunk)
        {
            label = messageCountLabel(QStringLiteral("Mark Not Junk"), queuedEmailCount);
        }
        else
        {
            const bool deleting = destination != mailboxes.end() &&
                                  destination->role == std::optional<std::string>{"trash"} &&
                                  intent.operation == MailboxSelectionOperation::Move;
            if (deleting)
            {
                label = messageCountLabel(QStringLiteral("Delete"), queuedEmailCount);
            }
            else
            {
                const auto verb = intent.operation == MailboxSelectionOperation::Move
                                      ? QStringLiteral("Move")
                                      : QStringLiteral("Copy");
                label = messageCountLabel(verb, queuedEmailCount);
                if (destination != mailboxes.end())
                    label += QStringLiteral(" to ") + QString::fromStdString(destination->name);
            }
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        javelin::app::undo::MailPatchHistory history;
        history.items.reserve(plan.mutations.size());
        for (const auto& mutation : plan.mutations)
        {
            const auto email =
                std::ranges::find(emails, mutation.emailId, &javelin::jmap::domain::Email::id);
            if (email == emails.end())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                    .message = i18n("A planned email mutation lost its cache snapshot."),
                };
            }
            history.items.push_back(historyItem(intent.accountId, *email, mutation));
        }

        auto preparedResult = m_undoManager.prepareNormal(
            label, javelin::app::undo::HistoryDomain::Mail, history, operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        if (!prepared.has_value())
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::LocalStorageFailure,
                .message = i18n("Failed to reserve operation history."),
            };
        }

        auto mutations = plan.mutations;
        for (auto& mutation : mutations)
            mutation.operationGroupId = operationGroupId.toStdString();
        auto queuedResult = queueExactEmailMutations(intent.accountId, std::move(mutations));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (const auto discardError = m_undoManager.discardNormal(prepared->entryId))
                return javelin::jmap::operationError(*discardError);
            return *error;
        }
        auto queuedMutations =
            std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(queuedResult));
        for (std::size_t index = 0; index < queuedMutations.size(); ++index)
            std::get<javelin::app::undo::MailPatchHistory>(prepared->payload)
                .items[index]
                .mutationId = queuedMutations[index].mutationId;

        auto committedResult = m_undoManager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committedResult))
            return javelin::jmap::operationError(*error);
        const auto& committed = std::get<javelin::app::undo::HistoryEntry>(committedResult);
        return QueuedMailboxSelectionMutation{
            .accountId = std::move(intent.accountId),
            .queuedEmailCount = queuedEmailCount,
            .skippedEmailCount = plan.skippedEmailCount,
            .queuedMutations = std::move(queuedMutations),
            .historyEntryId = committed.entryId,
        };
    }

    QueuedMessageSelectionMutationResult
    MailApplicationService::queueDestroyMessages(std::string accountId,
                                                 std::optional<std::string> sourceMailboxId,
                                                 MessageSelection selection)
    {
        return queueSelectedMessageMutation(std::move(accountId), std::move(sourceMailboxId),
                                            std::move(selection), SelectedMessageMutation::Destroy);
    }

    QueuedMessageSelectionMutationResult
    MailApplicationService::queueMarkMessagesUnread(std::string accountId,
                                                    std::optional<std::string> sourceMailboxId,
                                                    MessageSelection selection)
    {
        return queueSelectedMessageMutation(std::move(accountId), std::move(sourceMailboxId),
                                            std::move(selection),
                                            SelectedMessageMutation::MarkUnread);
    }

    QueuedMessageSelectionMutationResult MailApplicationService::queueSelectedMessageMutation(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, const SelectedMessageMutation mutation)
    {
        auto emailIdsResult =
            resolveMessageSelection(m_queryService, accountId, sourceMailboxId, selection);
        if (const auto* error = std::get_if<QString>(&emailIdsResult))
        {
            return javelin::jmap::OperationError{.message = *error};
        }

        auto emailIds = std::get<std::vector<std::string>>(std::move(emailIdsResult));
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        std::vector<javelin::jmap::domain::Email> selectedEmails;
        for (const auto& emailId : emailIds)
        {
            const auto found = emails.find(accountId, emailId);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
                return javelin::jmap::operationError(*error);
            const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
            if (!email.has_value())
            {
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("A selected message is not cached locally."),
                };
            }
            if (mutation == SelectedMessageMutation::MarkUnread &&
                !std::ranges::contains(email->keywords, std::string{"$seen"}))
            {
                continue;
            }
            selectedEmails.push_back(*email);
        }
        if (selectedEmails.empty())
        {
            return QueuedMessageSelectionMutation{
                .accountId = std::move(accountId),
                .queuedEmailCount = 0,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (mutation == SelectedMessageMutation::MarkUnread)
        {
            javelin::app::undo::MailPatchHistory history;
            for (const auto& email : selectedEmails)
            {
                history.items.push_back(
                    historyItem(accountId, email,
                                {
                                    .emailId = email.id,
                                    .addMailboxIds = {},
                                    .removeMailboxIds = {},
                                    .addKeywords = {},
                                    .removeKeywords = {"$seen"},
                                    .operationGroupId = operationGroupId.toStdString(),
                                    .ifInState = std::nullopt,
                                }));
            }
            auto preparedResult = m_undoManager.prepareNormal(
                messageCountLabel(QStringLiteral("Mark"), selectedEmails.size()) +
                    QStringLiteral(" Unread"),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }
        else
        {
            const auto count = selectedEmails.size();
            const auto explanation =
                count == 1
                    ? QStringLiteral("Unable to undo permanently deleting this message.")
                    : QStringLiteral("Unable to undo permanently deleting %1 messages.").arg(count);
            auto preparedResult = m_undoManager.prepareImpossible(
                messageCountLabel(QStringLiteral("Permanently Delete"), count),
                javelin::app::undo::HistoryDomain::Mail, explanation, operationGroupId);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }

        std::vector<javelin::jmap::EmailMailboxMutation> mutations;
        mutations.reserve(selectedEmails.size());
        for (const auto& email : selectedEmails)
        {
            mutations.push_back({
                .emailId = email.id,
                .addMailboxIds = {},
                .removeMailboxIds = {},
                .addKeywords = {},
                .removeKeywords = mutation == SelectedMessageMutation::MarkUnread
                                      ? std::vector<std::string>{"$seen"}
                                      : std::vector<std::string>{},
                .operationGroupId = operationGroupId.toStdString(),
                .ifInState = std::nullopt,
                .authoritativeMailboxIds = std::nullopt,
                .authoritativeKeywords = std::nullopt,
                .destroy = mutation == SelectedMessageMutation::Destroy,
            });
        }
        auto queuedResult = queueExactEmailMutations(accountId, std::move(mutations));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (prepared.has_value())
            {
                if (const auto discardError = m_undoManager.discardNormal(prepared->entryId))
                    return javelin::jmap::operationError(*discardError);
            }
            return *error;
        }
        auto queuedMutations =
            std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(queuedResult));
        if (mutation == SelectedMessageMutation::MarkUnread && prepared.has_value())
        {
            for (std::size_t index = 0; index < queuedMutations.size(); ++index)
                std::get<javelin::app::undo::MailPatchHistory>(prepared->payload)
                    .items[index]
                    .mutationId = queuedMutations[index].mutationId;
        }

        std::optional<QString> historyEntryId;
        if (prepared.has_value())
        {
            auto committed = mutation == SelectedMessageMutation::Destroy
                                 ? m_undoManager.commitImpossible(std::move(*prepared))
                                 : m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                return javelin::jmap::operationError(*error);
            historyEntryId = std::get<javelin::app::undo::HistoryEntry>(committed).entryId;
        }

        return QueuedMessageSelectionMutation{
            .accountId = std::move(accountId),
            .queuedEmailCount = selectedEmails.size(),
            .queuedMutations = std::move(queuedMutations),
            .historyEntryId = std::move(historyEntryId),
        };
    }

    QueuedMessageSelectionMutationResult
    MailApplicationService::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        const auto found = emails.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            return javelin::jmap::operationError(*error);
        const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
        if (!email.has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("Message not found."),
            };
        if (std::ranges::contains(email->keywords, std::string{"$seen"}))
        {
            return QueuedMessageSelectionMutation{
                .accountId = std::move(accountId),
                .queuedEmailCount = 0,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        javelin::jmap::EmailMailboxMutation mutation{
            .emailId = emailId,
            .addMailboxIds = {},
            .removeMailboxIds = {},
            .addKeywords = {"$seen"},
            .removeKeywords = {},
            .operationGroupId = operationGroupId.toStdString(),
            .ifInState = std::nullopt,
        };
        javelin::app::undo::MailPatchHistory history{
            .items = {historyItem(accountId, *email, mutation)}};
        auto preparedResult = m_undoManager.prepareNormal(QStringLiteral("Mark Message Read"),
                                                          javelin::app::undo::HistoryDomain::Mail,
                                                          std::move(history), operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto queuedResult = queueExactEmailMutation(accountId, std::move(mutation));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (prepared.has_value())
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
            return *error;
        }
        auto queued = std::get<javelin::jmap::QueuedEmailMutation>(std::move(queuedResult));
        std::get<javelin::app::undo::MailPatchHistory>(prepared->payload).items.front().mutationId =
            queued.mutationId;
        auto committed = m_undoManager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
            return javelin::jmap::operationError(*error);
        return QueuedMessageSelectionMutation{
            .accountId = std::move(accountId),
            .queuedEmailCount = 1,
            .queuedMutations = {std::move(queued)},
            .historyEntryId = std::get<javelin::app::undo::HistoryEntry>(committed).entryId,
        };
    }

    QueuedMessageSelectionMutationResult
    MailApplicationService::queueSetEmailFlagged(std::string accountId, std::string emailId,
                                                 const bool flagged)
    {
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        const auto found = emails.find(accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&found))
            return javelin::jmap::operationError(*error);
        const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(found);
        if (!email.has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("Message not found."),
            };
        const bool currentlyFlagged =
            std::ranges::contains(email->keywords, std::string{"$flagged"});
        if (currentlyFlagged == flagged)
        {
            return QueuedMessageSelectionMutation{
                .accountId = std::move(accountId),
                .queuedEmailCount = 0,
                .queuedMutations = {},
                .historyEntryId = std::nullopt,
            };
        }

        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        javelin::jmap::EmailMailboxMutation mutation{
            .emailId = emailId,
            .addMailboxIds = {},
            .removeMailboxIds = {},
            .addKeywords =
                flagged ? std::vector<std::string>{"$flagged"} : std::vector<std::string>{},
            .removeKeywords =
                flagged ? std::vector<std::string>{} : std::vector<std::string>{"$flagged"},
            .operationGroupId = operationGroupId.toStdString(),
            .ifInState = std::nullopt,
        };
        javelin::app::undo::MailPatchHistory history{
            .items = {historyItem(accountId, *email, mutation)}};
        auto preparedResult = m_undoManager.prepareNormal(
            flagged ? QStringLiteral("Add Star to Message")
                    : QStringLiteral("Remove Star from Message"),
            javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto queuedResult = queueExactEmailMutation(accountId, std::move(mutation));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&queuedResult))
        {
            if (prepared.has_value())
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
            return *error;
        }
        auto queued = std::get<javelin::jmap::QueuedEmailMutation>(std::move(queuedResult));
        std::get<javelin::app::undo::MailPatchHistory>(prepared->payload).items.front().mutationId =
            queued.mutationId;
        auto committed = m_undoManager.commitNormal(std::move(*prepared));
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
            return javelin::jmap::operationError(*error);
        return QueuedMessageSelectionMutation{
            .accountId = std::move(accountId),
            .queuedEmailCount = 1,
            .queuedMutations = {std::move(queued)},
            .historyEntryId = std::get<javelin::app::undo::HistoryEntry>(committed).entryId,
        };
    }

    javelin::jmap::QueuedEmailMutationResult
    MailApplicationService::queueExactEmailMutation(std::string accountId,
                                                    javelin::jmap::EmailMailboxMutation mutation)
    {
        auto result = queueExactEmailMutations(std::move(accountId), {std::move(mutation)});
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
            return *error;
        auto queued = std::get<std::vector<javelin::jmap::QueuedEmailMutation>>(std::move(result));
        return std::move(queued.front());
    }

    javelin::jmap::QueuedEmailMutationsResult MailApplicationService::queueExactEmailMutations(
        std::string accountId, std::vector<javelin::jmap::EmailMailboxMutation> mutations)
    {
        auto result = m_jmapCore.queueEmailMailboxMutations(accountId, std::move(mutations));
        const auto* queued = std::get_if<std::vector<javelin::jmap::QueuedEmailMutation>>(&result);
        if (queued == nullptr)
            return result;

        QStringList affectedMailboxIds;
        const auto appendMailboxIds = [&affectedMailboxIds](const auto& mailboxIds)
        {
            for (const auto& mailboxId : mailboxIds)
            {
                const auto value = QString::fromStdString(mailboxId);
                if (!affectedMailboxIds.contains(value))
                    affectedMailboxIds.push_back(value);
            }
        };
        for (const auto& mutation : *queued)
        {
            appendMailboxIds(mutation.patch.addMailboxIds);
            appendMailboxIds(mutation.patch.removeMailboxIds);
            if (mutation.patch.authoritativeMailboxIds.has_value())
                appendMailboxIds(*mutation.patch.authoritativeMailboxIds);
        }

        Q_EMIT cacheCommitted(MailCacheChange{
            .accountId = QString::fromStdString(accountId),
            .mailboxIds = std::move(affectedMailboxIds),
            .queryWindows = {},
            .searchWindows = {},
            .mailboxTreeChanged = false,
            .hasNewMail = false,
            .optimisticProjection = true,
        });
        return result;
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailApplicationService::submitPendingEmailMutations(std::string accountId,
                                                        std::optional<std::string> operationGroupId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        auto affectedMailboxIds = affectedMailboxIdsForPendingMutations(
            m_databaseConnection, accountId, operationGroupId);
        auto result = co_await m_jmapCore.submitPendingEmailMutations(
            toLiveConnectionSettings(configuration->second.settings), accountId, operationGroupId);

        if (operationGroupId.has_value())
        {
            const auto historyEntry = std::ranges::find_if(
                m_undoManager.entries(),
                [&](const javelin::app::undo::HistoryEntry& entry)
                {
                    return entry.operationGroupId.has_value() &&
                           entry.operationGroupId->toStdString() == *operationGroupId;
                });
            if (historyEntry != m_undoManager.entries().end())
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    using enum javelin::jmap::OperationErrorCode;
                    if (error->code == NetworkUnavailable || error->code == Timeout ||
                        error->code == HttpFailure || error->code == ProtocolViolation)
                    {
                        static_cast<void>(m_undoManager.setEntryStatus(
                            historyEntry->entryId,
                            javelin::app::undo::HistoryEntryStatus::BlockedUnknown,
                            error->message));
                    }
                }
                else
                {
                    const auto& submitted =
                        std::get<javelin::jmap::SubmittedEmailMutations>(result);
                    if (auto* history = std::get_if<javelin::app::undo::MailPatchHistory>(
                            &historyEntry->payload))
                    {
                        std::unordered_set<std::string> accepted;
                        for (const auto& item : submitted.items)
                        {
                            if (item.accepted)
                                accepted.insert(item.emailId);
                        }
                        auto updatedEntry = *historyEntry;
                        auto& updatedHistory =
                            std::get<javelin::app::undo::MailPatchHistory>(updatedEntry.payload);
                        std::erase_if(updatedHistory.items, [&](const auto& item)
                                      { return !accepted.contains(item.emailId); });
                        if (updatedHistory.items.empty())
                            static_cast<void>(m_undoManager.forget(updatedEntry.entryId));
                        else if (updatedHistory.items.size() != history->items.size())
                            static_cast<void>(m_undoManager.replaceEntry(std::move(updatedEntry)));
                    }
                    else if (std::holds_alternative<javelin::app::undo::ImpossibleHistory>(
                                 historyEntry->payload) &&
                             submitted.updatedEmailCount == 0)
                    {
                        static_cast<void>(m_undoManager.forget(historyEntry->entryId));
                    }
                }
            }
        }

        auto observed =
            observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                          QStringLiteral("Submit pending mail changes"), std::move(result));
        if (const auto* submitted = std::get_if<javelin::jmap::SubmittedEmailMutations>(&observed);
            submitted != nullptr && submitted->attemptedEmailCount > 0)
        {
            Q_EMIT cacheCommitted(MailCacheChange{
                .accountId = QString::fromStdString(accountId),
                .mailboxIds = std::move(affectedMailboxIds),
                .queryWindows = {},
                .searchWindows = {},
                .mailboxTreeChanged = false,
                .hasNewMail = false,
                .optimisticProjection = true,
            });
        }
        co_return observed;
    }

    QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
    MailApplicationService::getAuthoritativeEmails(std::string accountId,
                                                   std::vector<std::string> emailIds)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return co_await m_jmapCore.getAuthoritativeEmails(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(emailIds));
    }

    javelin::jmap::AuthoritativeEmailsResult
    MailApplicationService::getEffectiveEmails(const std::string_view accountId,
                                               const std::span<const std::string> emailIds)
    {
        javelin::jmap::cache::EmailRepository emails{m_databaseConnection};
        javelin::jmap::cache::SyncStateRepository states{m_databaseConnection};
        const auto stateResult = states.find(
            {.accountId = std::string{accountId}, .objectType = "Email", .queryKey = {}});
        const auto* state =
            std::get_if<std::optional<javelin::jmap::cache::SyncStateRecord>>(&stateResult);
        if (state == nullptr)
            return javelin::jmap::operationError(
                std::get<javelin::jmap::cache::DatabaseError>(stateResult));
        if (!state->has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Email state is not available offline."),
            };
        std::vector<javelin::jmap::domain::Email> result;
        result.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            const auto found = emails.find(accountId, emailId);
            const auto* email = std::get_if<std::optional<javelin::jmap::domain::Email>>(&found);
            if (email == nullptr)
                return javelin::jmap::operationError(
                    std::get<javelin::jmap::cache::DatabaseError>(found));
            if (email->has_value())
                result.push_back(**email);
        }
        return javelin::jmap::AuthoritativeEmails{
            .accountId = std::string{accountId},
            .state = state->value().stateToken,
            .emails = std::move(result),
            .notFound = {},
        };
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    MailApplicationService::requestMessageContent(std::string accountId, std::string emailId)
    {
        const auto maintenance = emailMaintenanceActive(
            m_databaseConnection, m_mailboxMaintenanceRegistry, accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(maintenance))
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        const ForegroundWorkScope foreground{m_workScheduler};
        auto result = observeResult(m_errorCoordinator, configuration->second.settings, accountId,
                                    QStringLiteral("Load message content"),
                                    co_await m_jmapCore.refreshMessageContent(
                                        toLiveConnectionSettings(configuration->second.settings),
                                        accountId, std::move(emailId)));
        if (std::holds_alternative<javelin::jmap::MessageContentUnavailable>(result))
            static_cast<void>(requestAccountSynchronization(accountId));
        co_return result;
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    MailApplicationService::requestAttachment(std::string accountId, std::string emailId,
                                              std::string partId)
    {
        const auto maintenance = emailMaintenanceActive(
            m_databaseConnection, m_mailboxMaintenanceRegistry, accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(maintenance))
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
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
        const auto maintenance = emailMaintenanceActive(
            m_databaseConnection, m_mailboxMaintenanceRegistry, accountId, emailId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&maintenance))
            co_return javelin::jmap::operationError(*error);
        if (std::get<bool>(maintenance))
            co_return javelin::jmap::OperationError{
                .message = i18n("The mailbox cache is being cleared."),
            };
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

    void MailApplicationService::scheduleContactRefresh(std::string ownerAccountId)
    {
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            return;

        m_pendingContactRefreshes.insert(ownerAccountId);
        const auto jobId = contactRefreshJobId(ownerAccountId);
        if (const auto error = m_workScheduler.ensure({
                .jobId = jobId,
                .parentJobId = std::nullopt,
                .accountId = ownerAccountId,
                .kind = WorkKind::ContactRefresh,
                .priority = WorkPriority::Freshness,
                .title = i18n("Refresh contacts"),
                .checkpointJson = QStringLiteral("{}"),
                .restartCompleted = true,
            }))
        {
            qWarning().noquote() << "Could not queue contact refresh" << error->message;
            return;
        }

        const auto current = m_workScheduler.find(jobId);
        const auto* job = std::get_if<std::optional<WorkRecord>>(&current);
        if (job == nullptr || !job->has_value())
        {
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&current))
                qWarning().noquote() << "Could not inspect contact refresh" << error->message;
            return;
        }

        if (m_errorCoordinator.authenticationPaused(configuration->second.settings.connectionId,
                                                    configuration->second.settings.revision))
        {
            if ((*job)->status != WorkStatus::Paused)
            {
                WorkProgress authenticationProgress;
                authenticationProgress.detail =
                    QStringLiteral("Waiting for account authentication");
                static_cast<void>(m_workScheduler.update(jobId, WorkStatus::WaitingForAuth,
                                                         authenticationProgress,
                                                         QStringLiteral("{}")));
            }
            return;
        }

        if ((*job)->status != WorkStatus::Queued && (*job)->status != WorkStatus::Running &&
            (*job)->status != WorkStatus::Paused)
        {
            static_cast<void>(
                m_workScheduler.update(jobId, WorkStatus::Queued, {}, QStringLiteral("{}")));
        }
        scheduleContactRefreshPump();
    }

    void MailApplicationService::restoreContactRefreshJobs()
    {
        const auto listed = m_workScheduler.list();
        const auto* jobs = std::get_if<std::vector<WorkRecord>>(&listed);
        if (jobs == nullptr)
        {
            qWarning() << "Could not restore queued contact refresh work";
            return;
        }
        for (const auto& job : *jobs)
        {
            if (job.kind != WorkKind::ContactRefresh || !job.accountId.has_value() ||
                !shouldRestoreContactRefresh(job.status) ||
                !m_configurations.contains(*job.accountId))
                continue;
            scheduleContactRefresh(*job.accountId);
        }
    }

    void MailApplicationService::scheduleContactRefreshPump()
    {
        if (m_pendingContactRefreshes.empty() || m_contactRefreshPumpScheduled)
            return;
        m_contactRefreshPumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this]()
                           {
                               m_contactRefreshPumpScheduled = false;
                               pumpContactRefreshes();
                           });
    }

    void MailApplicationService::pumpContactRefreshes()
    {
        if (!m_workScheduler.mayStartBackgroundNetwork())
            return;

        const std::vector<std::string> pending{m_pendingContactRefreshes.begin(),
                                               m_pendingContactRefreshes.end()};
        for (const auto& ownerAccountId : pending)
        {
            if (m_runningContactRefreshes.contains(ownerAccountId))
                continue;
            if (!m_configurations.contains(ownerAccountId))
            {
                m_pendingContactRefreshes.erase(ownerAccountId);
                continue;
            }

            const auto jobId = contactRefreshJobId(ownerAccountId);
            const auto current = m_workScheduler.find(jobId);
            const auto* job = std::get_if<std::optional<WorkRecord>>(&current);
            if (job == nullptr || !job->has_value() || (*job)->status != WorkStatus::Queued)
                continue;
            if (!m_workScheduler.admit(jobId).has_value())
                continue;

            m_pendingContactRefreshes.erase(ownerAccountId);
            m_runningContactRefreshes.insert(ownerAccountId);
            auto task = runContactRefresh(ownerAccountId, jobId);
            QCoro::connect(std::move(task), this,
                           [this, ownerAccountId, jobId]()
                           {
                               m_workScheduler.release(jobId);
                               m_runningContactRefreshes.erase(ownerAccountId);
                               if (m_pendingContactRefreshes.contains(ownerAccountId))
                               {
                                   const auto completedJobResult = m_workScheduler.find(jobId);
                                   const auto* completedJob =
                                       std::get_if<std::optional<WorkRecord>>(&completedJobResult);
                                   if (completedJob != nullptr && completedJob->has_value() &&
                                       ((*completedJob)->status == WorkStatus::Complete ||
                                        (*completedJob)->status == WorkStatus::Failed))
                                   {
                                       static_cast<void>(m_workScheduler.update(
                                           jobId, WorkStatus::Queued, {}, QStringLiteral("{}")));
                                   }
                               }
                               scheduleContactRefreshPump();
                           });
        }
    }

    QCoro::Task<void> MailApplicationService::runContactRefresh(std::string ownerAccountId,
                                                                std::string jobId)
    {
        WorkProgress progress;
        progress.detail = i18n("Checking for contact changes");
        static_cast<void>(
            m_workScheduler.update(jobId, WorkStatus::Running, progress, QStringLiteral("{}")));

        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
        {
            static_cast<void>(m_workScheduler.update(
                jobId, WorkStatus::Failed, progress, QStringLiteral("{}"),
                QStringLiteral("Account synchronization is not configured.")));
            co_return;
        }
        const auto settings = configuration->second.settings;
        auto result = observeResult(m_errorCoordinator, settings, ownerAccountId,
                                    QStringLiteral("Synchronize contacts after state change"),
                                    co_await m_contactService.refreshAll(
                                        toLiveConnectionSettings(settings), ownerAccountId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            WorkStatus status = WorkStatus::Failed;
            if (javelin::jmap::isAuthenticationError(*error))
                status = WorkStatus::WaitingForAuth;
            else if (javelin::jmap::isTransientError(*error))
                status = WorkStatus::WaitingForNetwork;
            progress.detail = error->message;
            static_cast<void>(m_workScheduler.update(jobId, status, progress, QStringLiteral("{}"),
                                                     error->message));

            if (status == WorkStatus::WaitingForAuth || status == WorkStatus::WaitingForNetwork)
                m_pendingContactRefreshes.insert(ownerAccountId);
            if (status == WorkStatus::WaitingForNetwork)
            {
                QTimer::singleShot(contactRefreshRetryDelay, this,
                                   [this, ownerAccountId, jobId]()
                                   {
                                       if (!m_pendingContactRefreshes.contains(ownerAccountId))
                                           return;
                                       const auto current = m_workScheduler.find(jobId);
                                       const auto* job =
                                           std::get_if<std::optional<WorkRecord>>(&current);
                                       if (job == nullptr || !job->has_value() ||
                                           (*job)->status != WorkStatus::WaitingForNetwork)
                                           return;
                                       static_cast<void>(m_workScheduler.update(
                                           jobId, WorkStatus::Queued, {}, QStringLiteral("{}")));
                                       scheduleContactRefreshPump();
                                   });
            }
            co_return;
        }

        const auto& summary = std::get<javelin::jmap::contacts::ContactRefreshSummary>(result);
        progress.completedUnits = summary.contactCount;
        progress.totalUnits = summary.contactCount;
        progress.detail = i18n("%1 contacts across %2 address books", summary.contactCount,
                               summary.addressBookCount);
        static_cast<void>(
            m_workScheduler.update(jobId, WorkStatus::Complete, progress, QStringLiteral("{}")));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    MailApplicationService::requestContacts(std::string accountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, accountId,
            i18n("Synchronize contacts"),
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
                .message = accountSynchronizationNotConfigured(),
            };
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
                                i18n("Synchronize calendar"), std::move(result));
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
                .message = i18n("Calendar synchronization is not configured.")};
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
                                i18n("Synchronize calendar changes"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::AuthoritativeCalendarEventResult>
    MailApplicationService::getAuthoritativeCalendarEvent(std::string ownerAccountId,
                                                          std::string accountId,
                                                          std::optional<std::string> eventId,
                                                          std::string uid)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return co_await m_calendarService.getAuthoritativeEvent(
            toLiveConnectionSettings(configuration->second.settings), std::move(ownerAccountId),
            std::move(accountId), std::move(eventId), std::move(uid));
    }

    javelin::jmap::calendar::AuthoritativeCalendarEventResult
    MailApplicationService::getEffectiveCalendarEvent(const std::string_view accountId,
                                                      const std::optional<std::string>& eventId)
    {
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        const auto stateResult = repository.stateToken(accountId, "CalendarEvent");
        const auto* state = std::get_if<std::optional<std::string>>(&stateResult);
        if (state == nullptr)
            return javelin::jmap::operationError(
                std::get<javelin::jmap::cache::DatabaseError>(stateResult));
        if (!state->has_value())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Calendar event state is not available offline."),
            };
        std::optional<javelin::jmap::calendar::CalendarEvent> event;
        if (eventId.has_value())
        {
            const auto found = repository.findEvent(accountId, *eventId);
            const auto* cached =
                std::get_if<std::optional<javelin::jmap::calendar::CalendarEvent>>(&found);
            if (cached == nullptr)
                return javelin::jmap::operationError(
                    std::get<javelin::jmap::cache::DatabaseError>(found));
            event = *cached;
        }
        return javelin::jmap::calendar::AuthoritativeCalendarEvent{
            .state = **state,
            .event = std::move(event),
        };
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::createCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::CreateEventCommand command,
                                                const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        if (command.event.uid.empty())
            command.event.uid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto afterDocument =
            javelin::jmap::api::serializeCalendarEventDocument(command.event);
        if (!afterDocument.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Unable to serialize the calendar event history."),
            };
        if (!command.operationGroupId.has_value())
            command.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto calendar = std::ranges::find_if(command.event.calendarIds,
                                                   [](const auto& value) { return value.second; });
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Create “%1”", QString::fromStdString(command.event.title)),
            javelin::app::undo::HistoryDomain::Calendar,
            javelin::app::undo::CalendarEventHistory{
                .connectionId = ownerAccountId,
                .accountId = command.accountId,
                .calendarId =
                    calendar == command.event.calendarIds.end() ? std::string{} : calendar->first,
                .currentEventId = std::nullopt,
                .uid = command.event.uid,
                .beforeDocumentJson = std::nullopt,
                .afterDocumentJson = afterDocument,
            },
            QString::fromStdString(*command.operationGroupId), std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
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
        auto result = co_await m_calendarService.create(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto& history = std::get<javelin::app::undo::CalendarEventHistory>(prepared->payload);
            const auto& committedMutation =
                std::get<javelin::jmap::calendar::CommittedMutation>(result);
            history.currentEventId = committedMutation.createdId;
            if (!history.currentEventId.has_value())
            {
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::ProtocolViolation,
                    .message = i18n("The created calendar event has no server ID."),
                };
            }
            auto authoritative = co_await m_calendarService.getAuthoritativeEvent(
                toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
                history.accountId, history.currentEventId, history.uid);
            if (const auto* accepted =
                    std::get_if<javelin::jmap::calendar::AuthoritativeCalendarEvent>(
                        &authoritative);
                accepted != nullptr && accepted->event.has_value() &&
                (committedMutation.newState.empty() ||
                 accepted->state == committedMutation.newState))
            {
                const auto acceptedDocument =
                    javelin::jmap::api::serializeCalendarEventDocument(*accepted->event);
                if (acceptedDocument.has_value())
                    history.afterDocumentJson = acceptedDocument;
            }
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Create calendar event"), std::move(result));
    }

    javelin::jmap::calendar::CalendarPreferenceResult
    MailApplicationService::setCalendarVisible(std::string accountId, std::string calendarId,
                                               const bool visible,
                                               const javelin::app::undo::CommandOrigin origin)
    {
        const auto listed = m_calendarService.calendars(accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        const auto calendar =
            std::ranges::find(calendars, calendarId, &javelin::jmap::calendar::Calendar::id);
        if (calendar == calendars.end())
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar is no longer available."),
            };
        if (calendar->isVisible == visible)
            return std::monostate{};
        auto preparedResult =
            m_undoManager.prepareNormal(visible ? i18n("Show Calendar") : i18n("Hide Calendar"),
                                        javelin::app::undo::HistoryDomain::LocalPreference,
                                        javelin::app::undo::CalendarPreferenceHistory{
                                            .connectionId = {},
                                            .accountId = accountId,
                                            .preferenceKind = "visibility",
                                            .objectId = calendarId,
                                            .beforeValue = calendar->isVisible ? "true" : "false",
                                            .afterValue = visible ? "true" : "false",
                                        },
                                        std::nullopt, std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto result = m_calendarService.setCalendarVisible(accountId, calendarId, visible);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (prepared.has_value())
                static_cast<void>(m_undoManager.discardNormal(prepared->entryId));
            return *error;
        }
        if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                return javelin::jmap::operationError(*error);
        }
        return std::monostate{};
    }

    std::variant<std::optional<std::string>, javelin::jmap::OperationError>
    MailApplicationService::currentCalendarPreference(
        const javelin::app::undo::CalendarPreferenceHistory& history) const
    {
        const auto listed = m_calendarService.calendars(history.accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        if (history.preferenceKind == "default_calendar")
        {
            const auto current =
                std::ranges::find(calendars, true, &javelin::jmap::calendar::Calendar::isDefault);
            return current == calendars.end() ? std::optional<std::string>{std::nullopt}
                                              : std::optional<std::string>{current->id};
        }
        if (history.preferenceKind == "visibility" || history.preferenceKind == "subscription")
        {
            const auto calendar = std::ranges::find(calendars, history.objectId,
                                                    &javelin::jmap::calendar::Calendar::id);
            if (calendar == calendars.end())
                return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::NotFound,
                    .message = i18n("The calendar is no longer available."),
                };
            const bool enabled = history.preferenceKind == "visibility" ? calendar->isVisible
                                                                        : calendar->isSubscribed;
            return std::optional<std::string>{enabled ? "true" : "false"};
        }
        return javelin::jmap::OperationError{
            .code = javelin::jmap::OperationErrorCode::InvalidRequest,
            .message = i18n("Unknown calendar preference history."),
        };
    }

    QCoro::Task<std::optional<javelin::jmap::OperationError>>
    MailApplicationService::applyCalendarPreference(
        javelin::app::undo::CalendarPreferenceHistory history, std::optional<std::string> value,
        const javelin::app::undo::CommandOrigin origin)
    {
        if (history.preferenceKind == "visibility")
        {
            if (!value.has_value() || (*value != "true" && *value != "false"))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                    .message = i18n("The calendar visibility history is invalid."),
                };
            auto result =
                setCalendarVisible(history.accountId, history.objectId, *value == "true", origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            co_return std::nullopt;
        }
        if (history.preferenceKind == "subscription")
        {
            if (!value.has_value() || (*value != "true" && *value != "false"))
                co_return javelin::jmap::OperationError{
                    .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                    .message = i18n("The calendar subscription history is invalid."),
                };
            auto result =
                co_await setCalendarSubscribed(history.connectionId, history.accountId,
                                               history.objectId, *value == "true", origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            co_return std::nullopt;
        }
        if (history.preferenceKind == "default_calendar" && value.has_value())
        {
            auto result = co_await setDefaultCalendar(history.connectionId, history.accountId,
                                                      *value, origin);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                co_return *error;
            co_return std::nullopt;
        }
        co_return javelin::jmap::OperationError{
            .code = javelin::jmap::OperationErrorCode::InvalidRequest,
            .message = i18n("The calendar preference history is invalid."),
        };
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::setCalendarSubscribed(std::string ownerAccountId, std::string accountId,
                                                  std::string calendarId, const bool subscribed,
                                                  const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto listed = m_calendarService.calendars(accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            co_return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        const auto calendar =
            std::ranges::find(calendars, calendarId, &javelin::jmap::calendar::Calendar::id);
        if (calendar == calendars.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar is no longer available."),
            };
        if (calendar->isSubscribed == subscribed)
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = accountId,
                .newState = {},
                .createdId = std::nullopt,
                .receipt = {},
            };

        auto preparedResult = m_undoManager.prepareNormal(
            subscribed ? i18n("Subscribe to Calendar") : i18n("Unsubscribe from Calendar"),
            javelin::app::undo::HistoryDomain::LocalPreference,
            javelin::app::undo::CalendarPreferenceHistory{
                .connectionId = ownerAccountId,
                .accountId = accountId,
                .preferenceKind = "subscription",
                .objectId = calendarId,
                .beforeValue = calendar->isSubscribed ? "true" : "false",
                .afterValue = subscribed ? "true" : "false",
            },
            std::nullopt, std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto result = co_await m_calendarService.setCalendarSubscribed(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId, accountId,
            std::move(calendarId), subscribed);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
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
                                i18n("Change calendar subscription"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::setDefaultCalendar(std::string ownerAccountId, std::string accountId,
                                               std::string calendarId,
                                               const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto listed = m_calendarService.calendars(accountId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
            co_return *error;
        const auto& calendars = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
        const auto current =
            std::ranges::find(calendars, true, &javelin::jmap::calendar::Calendar::isDefault);
        const std::optional<std::string> before =
            current == calendars.end() ? std::nullopt : std::optional{current->id};
        if (before == std::optional{calendarId})
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = accountId,
                .newState = {},
                .createdId = std::nullopt,
                .receipt = {},
            };
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Change Default Calendar"), javelin::app::undo::HistoryDomain::LocalPreference,
            javelin::app::undo::CalendarPreferenceHistory{
                .connectionId = ownerAccountId,
                .accountId = accountId,
                .preferenceKind = "default_calendar",
                .objectId = "default",
                .beforeValue = before,
                .afterValue = calendarId,
            },
            std::nullopt, std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
        auto result = co_await m_calendarService.setDefaultCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId, accountId,
            std::move(calendarId));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
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
                                i18n("Change default calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::createCalendar(std::string ownerAccountId,
                                           javelin::jmap::calendar::CreateCalendarCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarService.createCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command));
        Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                       .interval = {},
                                       .displayTimeZone = {},
                                       .accountCount = 1,
                                       .eventCount = 0});
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Create calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::deleteCalendar(std::string ownerAccountId,
                                           javelin::jmap::calendar::DeleteCalendarCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        auto result = co_await m_calendarService.deleteCalendar(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command));
        Q_EMIT calendarCacheCommitted({.ownerAccountId = QString::fromStdString(ownerAccountId),
                                       .interval = {},
                                       .displayTimeZone = {},
                                       .accountCount = 1,
                                       .eventCount = 0});
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Delete calendar"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::updateCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::UpdateEventCommand command,
                                                const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        const auto cached = repository.findEvent(command.accountId, command.event.id);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            co_return javelin::jmap::operationError(*error);
        const auto& before =
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);
        if (!before.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar event is no longer in the cache."),
            };
        if (*before == command.event)
            co_return javelin::jmap::calendar::CommittedMutation{
                .accountId = command.accountId,
                .newState = command.ifInState.value_or(std::string{}),
                .createdId = std::nullopt,
                .receipt = {},
            };
        const auto beforeDocument = javelin::jmap::api::serializeCalendarEventDocument(*before);
        const auto afterDocument =
            javelin::jmap::api::serializeCalendarEventDocument(command.event);
        if (!beforeDocument.has_value() || !afterDocument.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Unable to serialize the calendar event history."),
            };
        if (!command.operationGroupId.has_value())
            command.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto calendar = std::ranges::find_if(command.event.calendarIds,
                                                   [](const auto& value) { return value.second; });
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Edit “%1”", QString::fromStdString(command.event.title)),
            javelin::app::undo::HistoryDomain::Calendar,
            javelin::app::undo::CalendarEventHistory{
                .connectionId = ownerAccountId,
                .accountId = command.accountId,
                .calendarId =
                    calendar == command.event.calendarIds.end() ? std::string{} : calendar->first,
                .currentEventId = command.event.id,
                .uid = command.event.uid,
                .beforeDocumentJson = beforeDocument,
                .afterDocumentJson = afterDocument,
            },
            QString::fromStdString(*command.operationGroupId), std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
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
        auto result = co_await m_calendarService.update(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Update calendar event"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::respondToCalendarEvent(
        std::string ownerAccountId, javelin::jmap::calendar::RespondToEventCommand command)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
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
        auto result = co_await m_calendarService.respond(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Respond to calendar invitation"), std::move(result));
    }

    QCoro::Task<javelin::jmap::calendar::CalendarMutationResult>
    MailApplicationService::deleteCalendarEvent(std::string ownerAccountId,
                                                javelin::jmap::calendar::DeleteEventCommand command,
                                                const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        javelin::jmap::cache::CalendarRepository repository{m_databaseConnection};
        const auto cached = repository.findEvent(command.accountId, command.eventId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&cached))
            co_return javelin::jmap::operationError(*error);
        const auto& before =
            std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(cached);
        if (!before.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NotFound,
                .message = i18n("The calendar event is no longer in the cache."),
            };
        const auto beforeDocument = javelin::jmap::api::serializeCalendarEventDocument(*before);
        if (!beforeDocument.has_value())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidRequest,
                .message = i18n("Unable to serialize the calendar event history."),
            };
        if (!command.operationGroupId.has_value())
            command.operationGroupId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        const auto calendar = std::ranges::find_if(before->calendarIds,
                                                   [](const auto& value) { return value.second; });
        auto preparedResult = m_undoManager.prepareNormal(
            i18n("Delete “%1”", QString::fromStdString(before->title)),
            javelin::app::undo::HistoryDomain::Calendar,
            javelin::app::undo::CalendarEventHistory{
                .connectionId = ownerAccountId,
                .accountId = command.accountId,
                .calendarId =
                    calendar == before->calendarIds.end() ? std::string{} : calendar->first,
                .currentEventId = command.eventId,
                .uid = before->uid,
                .beforeDocumentJson = beforeDocument,
                .afterDocumentJson = std::nullopt,
            },
            QString::fromStdString(*command.operationGroupId), std::nullopt, origin);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
            co_return javelin::jmap::operationError(*error);
        auto prepared =
            std::get<std::optional<javelin::app::undo::HistoryEntry>>(std::move(preparedResult));
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
        auto result = co_await m_calendarService.remove(
            toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
            std::move(command), projectionCommitted);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
        }
        else if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* historyError =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*historyError);
        }
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Delete calendar event"), std::move(result));
    }

    QCoro::Task<javelin::jmap::sieve::SieveListResult>
    MailApplicationService::requestSieveScripts(std::string ownerAccountId)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            i18n("Load Sieve scripts"),
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
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            i18n("Load Sieve script"),
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
                .message = accountSynchronizationNotConfigured(),
            };
        co_return observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                                i18n("Validate Sieve script"),
                                co_await m_sieveService.validate(
                                    toLiveConnectionSettings(configuration->second.settings),
                                    ownerAccountId, std::move(content)));
    }

    QCoro::Task<javelin::jmap::sieve::SieveSaveResult> MailApplicationService::saveSieveScript(
        std::string ownerAccountId, javelin::jmap::sieve::SieveScript script, QByteArray content,
        const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            std::optional<std::string> beforeContent;
            if (!script.id.empty())
            {
                auto loaded = co_await m_sieveService.load(
                    toLiveConnectionSettings(configuration->second.settings), ownerAccountId,
                    script);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                    co_return *error;
                beforeContent = std::get<QByteArray>(loaded).toStdString();
            }
            javelin::app::undo::SieveHistory history{
                .connectionId = configuration->second.settings.connectionId,
                .accountId = ownerAccountId,
                .currentScriptId = script.id.empty() ? std::nullopt : std::optional{script.id},
                .previousScriptId = std::nullopt,
                .beforeName = script.id.empty() ? std::nullopt : std::optional{script.name},
                .beforeContent = std::move(beforeContent),
                .afterName = script.name,
                .afterContent = content.toStdString(),
                .activeScriptIdBefore = script.isActive ? std::optional{script.id} : std::nullopt,
                .activeScriptIdAfter = script.isActive ? std::optional{script.id} : std::nullopt,
            };
            auto preparedResult = m_undoManager.prepareNormal(
                script.id.empty()
                    ? i18n("Create Sieve Script “%1”", QString::fromStdString(script.name))
                    : i18n("Edit Sieve Script “%1”", QString::fromStdString(script.name)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }

        auto result = observeResult(
            m_errorCoordinator, configuration->second.settings, ownerAccountId,
            i18n("Save Sieve script"),
            co_await m_sieveService.save(toLiveConnectionSettings(configuration->second.settings),
                                         ownerAccountId, std::move(script), std::move(content),
                                         operationGroupId.toStdString()));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto& history = std::get<javelin::app::undo::SieveHistory>(prepared->payload);
            const auto& saved = std::get<javelin::jmap::sieve::SieveScript>(result);
            history.currentScriptId = saved.id;
            if (history.activeScriptIdBefore.has_value())
            {
                history.activeScriptIdBefore = saved.id;
                history.activeScriptIdAfter = saved.id;
            }
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*error);
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::sieve::SieveDeleteResult>
    MailApplicationService::deleteSieveScript(std::string ownerAccountId,
                                              javelin::jmap::sieve::SieveScript script,
                                              const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            auto loaded = co_await m_sieveService.load(
                toLiveConnectionSettings(configuration->second.settings), ownerAccountId, script);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                co_return *error;
            javelin::app::undo::SieveHistory history{
                .connectionId = configuration->second.settings.connectionId,
                .accountId = ownerAccountId,
                .currentScriptId = script.id,
                .previousScriptId = script.id,
                .beforeName = script.name,
                .beforeContent = std::get<QByteArray>(loaded).toStdString(),
                .afterName = std::nullopt,
                .afterContent = std::nullopt,
                .activeScriptIdBefore = script.isActive ? std::optional{script.id} : std::nullopt,
                .activeScriptIdAfter = std::nullopt,
            };
            auto preparedResult = m_undoManager.prepareNormal(
                i18n("Delete Sieve Script “%1”", QString::fromStdString(script.name)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }
        auto result =
            observeResult(m_errorCoordinator, configuration->second.settings, ownerAccountId,
                          i18n("Delete Sieve script"),
                          co_await m_sieveService.remove(
                              toLiveConnectionSettings(configuration->second.settings),
                              ownerAccountId, std::move(script), operationGroupId.toStdString()));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto& history = std::get<javelin::app::undo::SieveHistory>(prepared->payload);
            history.currentScriptId = std::nullopt;
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*error);
        }
        co_return result;
    }

    QCoro::Task<javelin::jmap::sieve::SieveActivationResult>
    MailApplicationService::setSieveScriptActive(std::string ownerAccountId,
                                                 javelin::jmap::sieve::SieveScript script,
                                                 const bool active,
                                                 const javelin::app::undo::CommandOrigin origin)
    {
        const ForegroundWorkScope foreground{m_workScheduler};
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::AuthenticationRequired,
                .message = accountSynchronizationNotConfigured(),
            };
        const auto operationGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        std::optional<javelin::app::undo::HistoryEntry> prepared;
        if (origin == javelin::app::undo::CommandOrigin::User)
        {
            auto listed = co_await m_sieveService.list(
                toLiveConnectionSettings(configuration->second.settings), ownerAccountId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&listed))
                co_return *error;
            const auto& scripts = std::get<std::vector<javelin::jmap::sieve::SieveScript>>(listed);
            const auto currentActive =
                std::ranges::find(scripts, true, &javelin::jmap::sieve::SieveScript::isActive);
            const std::optional<std::string> activeBefore =
                currentActive == scripts.end() ? std::nullopt : std::optional{currentActive->id};
            const std::optional<std::string> activeAfter =
                active ? std::optional{script.id} : std::nullopt;
            if (activeBefore == activeAfter)
                co_return std::monostate{};
            javelin::app::undo::SieveHistory history{
                .connectionId = configuration->second.settings.connectionId,
                .accountId = ownerAccountId,
                .currentScriptId = script.id,
                .previousScriptId = std::nullopt,
                .beforeName = std::nullopt,
                .beforeContent = std::nullopt,
                .afterName = std::nullopt,
                .afterContent = std::nullopt,
                .activeScriptIdBefore = activeBefore,
                .activeScriptIdAfter = activeAfter,
            };
            auto preparedResult = m_undoManager.prepareNormal(
                active ? i18n("Activate Sieve Script “%1”", QString::fromStdString(script.name))
                       : i18n("Deactivate Sieve Script “%1”", QString::fromStdString(script.name)),
                javelin::app::undo::HistoryDomain::Mail, std::move(history), operationGroupId,
                std::nullopt, origin);
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&preparedResult))
                co_return javelin::jmap::operationError(*error);
            prepared = std::get<std::optional<javelin::app::undo::HistoryEntry>>(
                std::move(preparedResult));
        }
        auto result = observeResult(m_errorCoordinator, configuration->second.settings,
                                    ownerAccountId, i18n("Change active Sieve script"),
                                    co_await m_sieveService.setActive(
                                        toLiveConnectionSettings(configuration->second.settings),
                                        ownerAccountId, std::move(script), active,
                                        operationGroupId.toStdString()));
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
        {
            if (const auto historyError = retainUnknownOrDiscard(m_undoManager, prepared, *error))
                co_return *historyError;
            co_return *error;
        }
        if (prepared.has_value())
        {
            auto committed = m_undoManager.commitNormal(std::move(*prepared));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&committed))
                co_return javelin::jmap::operationError(*error);
        }
        co_return result;
    }

    void MailApplicationService::connectCoordinator(const std::string& accountId,
                                                    AccountSyncCoordinator& coordinator)
    {
        connect(&coordinator, &AccountSyncCoordinator::statusChanged, this,
                [this, accountId](const auto status)
                { Q_EMIT accountStatusChanged(QString::fromStdString(accountId), status); });
        connect(&coordinator, &AccountSyncCoordinator::cacheCommitted, this,
                &MailApplicationService::cacheCommitted);
        connect(&coordinator, &AccountSyncCoordinator::contactStateChanged, this,
                [this](const QString& ownerAccountId, const auto& changedStates)
                {
                    static_cast<void>(changedStates);
                    scheduleContactRefresh(ownerAccountId.toStdString());
                });
        connect(&coordinator, &AccountSyncCoordinator::identityStateChanged, this,
                [this](const QString& ownerAccountId, const auto& changedStates)
                {
                    static_cast<void>(ownerAccountId);
                    for (const auto& [changedAccountId, states] : changedStates)
                    {
                        if (states.contains("Identity"))
                            Q_EMIT senderIdentityStateChanged(
                                QString::fromStdString(changedAccountId));
                    }
                });
        connect(&coordinator, &AccountSyncCoordinator::calendarStateChanged, this,
                [this](const QString& ownerAccountId, const auto& changedStates)
                {
                    static_cast<void>(changedStates);
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
