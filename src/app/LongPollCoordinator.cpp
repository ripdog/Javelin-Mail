#include "app/LongPollCoordinator.h"

#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/MailboxWindowPolicy.h"

#include <QCoroTask>

#include <QDebug>

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace javelin::app
{

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
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::QueryService& queryService,
        javelin::jmap::contacts::ContactService& contactService, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_jmapCore(jmapCore),
          m_methodTransport(methodTransport), m_networkAccessManager(networkAccessManager),
          m_accountRepository(accountRepository), m_queryService(queryService),
          m_contactService(contactService)
    {
    }

    void MailApplicationService::applySettings(std::vector<AccountSyncConfiguration> configurations)
    {
        std::unordered_set<std::string> configuredAccountIds;
        for (auto& configuration : configurations)
        {
            configuredAccountIds.insert(configuration.accountId);
            const auto accountId = configuration.accountId;
            m_configurations.insert_or_assign(accountId, std::move(configuration));
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
        refreshConfiguredSessions();
    }

    void MailApplicationService::refreshConfiguredSessions()
    {
        javelin::jmap::cache::SessionRepository sessions{m_databaseConnection};
        for (const auto& [accountId, configuration] : m_configurations)
        {
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

        const auto loginEmail = settings.loginEmail;
        const auto sessionUrl = settings.sessionUrl;
        auto task = m_jmapCore.refreshSession(toLiveConnectionSettings(settings), ownerAccountId);
        QCoro::connect(
            std::move(task), this,
            [this, ownerAccountId, loginEmail,
             sessionUrl](javelin::jmap::SessionRefreshResult result)
            {
                m_sessionRefreshesInFlight.erase(ownerAccountId);
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
                {
                    qWarning().noquote()
                        << "JMAP startup session discovery failed"
                        << QString::fromStdString(ownerAccountId) << error->message;
                    return;
                }

                const auto& summary = std::get<javelin::jmap::SessionRefreshSummary>(result);
                qInfo().noquote() << "JMAP startup session discovered"
                                  << QString::fromStdString(ownerAccountId)
                                  << (summary.websocketAdvertised ? QStringLiteral("WebSocket")
                                                                  : QStringLiteral("HTTP only"));
                for (const auto& [accountId, configuration] : m_configurations)
                {
                    if (configuration.settings.loginEmail == loginEmail &&
                        configuration.settings.sessionUrl == sessionUrl)
                    {
                        applyAccountConfiguration(accountId);
                    }
                }
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
                m_accountRepository, m_queryService, this);
            connectCoordinator(coordinatorIt->first, *coordinatorIt->second);
        }
        coordinatorIt->second->applySettings(std::move(configuration.settings), accountId,
                                             std::move(configuration.mailboxIds));
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

    QCoro::Task<MailboxWindowResult>
    MailApplicationService::requestMailboxWindow(MailboxWindowIntent intent)
    {
        const auto configuration = m_configurations.find(intent.accountId);
        if (configuration == m_configurations.end())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        }

        const auto cachedCountResult =
            m_queryService.countMailboxMessages(intent.accountId, intent.mailboxId);
        const auto mailboxTreeResult = m_queryService.listMailboxTree(intent.accountId);
        const auto* cachedCount = std::get_if<std::size_t>(&cachedCountResult);
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxTreeResult);
        if (cachedCount != nullptr && mailboxes != nullptr)
        {
            const auto mailbox =
                std::ranges::find(mailboxes->cbegin(), mailboxes->cend(), intent.mailboxId,
                                  &javelin::jmap::cache::MailboxTreeItem::id);
            if (mailbox != mailboxes->cend())
            {
                const javelin::jmap::sync::MailboxWindowAvailability availability{
                    .cachedRepresentativeCount = *cachedCount,
                    .serverRepresentativeCount = static_cast<std::size_t>(mailbox->totalThreads),
                    .offset = intent.offset,
                    .limit = intent.limit,
                    .sort = intent.sort,
                    .forceRefresh = intent.forceRefresh,
                };
                if (javelin::jmap::sync::cacheSatisfiesMailboxWindow(availability))
                {
                    co_return MailboxWindowSummary{
                        .accountId = std::move(intent.accountId),
                        .mailboxId = std::move(intent.mailboxId),
                        .offset = intent.offset,
                        .limit = intent.limit,
                        .representativeCount =
                            javelin::jmap::sync::cachedMailboxWindowSize(availability),
                        .total = availability.serverRepresentativeCount,
                    };
                }
            }
        }

        auto result = co_await m_jmapCore.queryMailboxPage(
            toLiveConnectionSettings(configuration->second.settings), intent.accountId,
            intent.mailboxId, intent.offset, intent.limit, intent.sort);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
        {
            co_return *error;
        }

        auto page = std::get<javelin::jmap::MailboxPageSummary>(std::move(result));
        MailboxWindowSummary summary{
            .accountId = page.accountId,
            .mailboxId = page.mailboxId,
            .offset = page.offset,
            .limit = page.limit,
            .representativeCount = page.representativeCount,
            .total = page.total,
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
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        }

        const auto queryKey = javelin::jmap::search::cacheKey(intent.criteria, intent.sort);
        auto result = co_await m_jmapCore.searchMessages(
            toLiveConnectionSettings(configuration->second.settings), intent.accountId,
            intent.criteria, intent.offset, intent.limit, intent.sort);
        if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
        {
            co_return *error;
        }

        const auto& page = std::get<javelin::jmap::MessageSearchSummary>(result);
        SearchWindowSummary summary{
            .accountId = page.accountId,
            .queryKey = queryKey,
            .offset = page.offset,
            .limit = page.limit,
            .representativeCount = page.representativeCount,
            .total = page.total,
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
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.submitPendingEmailMutations(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId));
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    MailApplicationService::requestMessageContent(std::string accountId, std::string emailId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.refreshMessageContent(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(emailId));
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    MailApplicationService::requestAttachment(std::string accountId, std::string emailId,
                                              std::string partId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.downloadAttachment(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(emailId), std::move(partId));
    }

    QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
    MailApplicationService::requestMessageSource(std::string accountId, std::string emailId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.downloadMessageSource(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(emailId));
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    MailApplicationService::bootstrapAccount(AccountBootstrapIntent intent)
    {
        co_return co_await m_jmapCore.refreshFromServer(toLiveConnectionSettings(intent.settings),
                                                        {}, std::move(intent.mailboxIds));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    MailApplicationService::requestContacts(std::string accountId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_contactService.refreshAll(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::setAddressBooks(std::string accountId,
                                            javelin::jmap::api::AddressBookSetRequest request)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.setAddressBooks(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::setContactCards(std::string accountId,
                                            javelin::jmap::api::ContactCardSetRequest request)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.setContactCards(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    MailApplicationService::copyContactCards(std::string accountId,
                                             javelin::jmap::api::ContactCardCopyRequest request)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.copyContactCards(
            toLiveConnectionSettings(configuration->second.settings), std::move(accountId),
            std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
    MailApplicationService::uploadContactMedia(std::string ownerAccountId, std::string accountId,
                                               QByteArray payload, std::string mediaType)
    {
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.uploadMedia(
            toLiveConnectionSettings(configuration->second.settings), std::move(ownerAccountId),
            std::move(accountId), std::move(payload), std::move(mediaType));
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
        connect(&coordinator, &AccountSyncCoordinator::notificationRaised, this,
                &MailApplicationService::notificationRaised);
    }

} // namespace javelin::app
