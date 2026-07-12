#include "app/LongPollCoordinator.h"

#include <ranges>
#include <unordered_set>

namespace javelin::app
{

    LongPollCoordinator::LongPollCoordinator(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::JmapCore& jmapCore, javelin::jmap::api::AbstractTransport& transport,
        QNetworkAccessManager& networkAccessManager,
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::QueryService& queryService,
        javelin::jmap::contacts::ContactService& contactService, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_jmapCore(jmapCore),
          m_transport(transport), m_networkAccessManager(networkAccessManager),
          m_accountRepository(accountRepository), m_queryService(queryService),
          m_contactService(contactService)
    {
    }

    void
    LongPollCoordinator::applySettings(std::vector<LongPollAccountConfiguration> configurations)
    {
        std::unordered_set<std::string> configuredAccountIds;
        for (auto& configuration : configurations)
        {
            configuredAccountIds.insert(configuration.accountId);
            const auto accountId = configuration.accountId;
            m_configurations.insert_or_assign(accountId, std::move(configuration));
            applyAccountConfiguration(accountId);
        }

        for (auto serviceIt = m_services.begin(); serviceIt != m_services.end();)
        {
            if (configuredAccountIds.contains(serviceIt->first))
            {
                ++serviceIt;
                continue;
            }

            Q_EMIT accountStatusChanged(QString::fromStdString(serviceIt->first),
                                        LongPollService::Status::Disconnected);
            disconnect(serviceIt->second.get(), nullptr, this, nullptr);
            serviceIt = m_services.erase(serviceIt);
        }
    }

    void LongPollCoordinator::applyAccountConfiguration(const std::string& accountId)
    {
        const auto stored = m_configurations.find(accountId);
        if (stored == m_configurations.end())
            return;

        auto configuration = stored->second;
        for (const auto& [id, observation] : m_observations)
        {
            static_cast<void>(id);
            if (observation.accountId == accountId &&
                std::ranges::find(configuration.mailboxIds, observation.mailboxId) ==
                    configuration.mailboxIds.end())
                configuration.mailboxIds.push_back(observation.mailboxId);
        }

        auto [serviceIt, inserted] = m_services.try_emplace(accountId);
        if (inserted)
        {
            serviceIt->second = std::make_unique<LongPollService>(
                m_databaseConnection, m_transport, m_networkAccessManager, m_accountRepository,
                m_queryService, this);
            connectService(serviceIt->first, *serviceIt->second);
        }
        serviceIt->second->applySettings(std::move(configuration.settings), accountId,
                                         std::move(configuration.mailboxIds));
    }

    std::uint64_t LongPollCoordinator::observeMailbox(std::string accountId, std::string mailboxId)
    {
        const auto id = m_nextObservationId++;
        const auto account = accountId;
        m_observations.emplace(id, MailboxObservation{.accountId = std::move(accountId),
                                                      .mailboxId = std::move(mailboxId)});
        applyAccountConfiguration(account);
        return id;
    }

    void LongPollCoordinator::unobserveMailbox(const std::uint64_t observationId)
    {
        const auto found = m_observations.find(observationId);
        if (found == m_observations.end())
            return;
        const auto accountId = found->second.accountId;
        m_observations.erase(found);
        applyAccountConfiguration(accountId);
    }

    bool LongPollCoordinator::requestAccountSynchronization(const std::string_view accountId)
    {
        const auto service = m_services.find(std::string{accountId});
        if (service == m_services.end())
            return false;
        return service->second->requestSynchronization();
    }

    QString LongPollCoordinator::statusSummary() const
    {
        return m_jmapCore.statusSummary();
    }

    QCoro::Task<javelin::jmap::MailboxPageResult>
    LongPollCoordinator::requestMailboxWindow(MailboxWindowIntent intent)
    {
        const auto configuration = m_configurations.find(intent.accountId);
        if (configuration == m_configurations.end())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        }
        co_return co_await m_jmapCore.queryMailboxPage(configuration->second.settings,
                                                       intent.accountId, intent.mailboxId,
                                                       intent.offset, intent.limit, intent.sort);
    }

    QCoro::Task<javelin::jmap::MessageSearchResult>
    LongPollCoordinator::requestSearchWindow(SearchWindowIntent intent)
    {
        const auto configuration = m_configurations.find(intent.accountId);
        if (configuration == m_configurations.end())
        {
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        }
        co_return co_await m_jmapCore.searchMessages(configuration->second.settings,
                                                     intent.accountId, intent.criteria,
                                                     intent.offset, intent.limit, intent.sort);
    }

    javelin::jmap::QueuedEmailMutationResult
    LongPollCoordinator::queueDestroyEmail(std::string accountId, std::string emailId)
    {
        return m_jmapCore.queueDestroyEmail(std::move(accountId), std::move(emailId));
    }

    javelin::jmap::QueuedEmailMutationResult
    LongPollCoordinator::queueMoveEmail(std::string accountId, std::string emailId,
                                        std::string sourceMailboxId,
                                        std::string destinationMailboxId)
    {
        return m_jmapCore.queueMoveEmail(std::move(accountId), std::move(emailId),
                                         std::move(sourceMailboxId),
                                         std::move(destinationMailboxId));
    }

    javelin::jmap::QueuedEmailMutationResult
    LongPollCoordinator::queueCopyEmail(std::string accountId, std::string emailId,
                                        std::string sourceMailboxId,
                                        std::string destinationMailboxId)
    {
        return m_jmapCore.queueCopyEmail(std::move(accountId), std::move(emailId),
                                         std::move(sourceMailboxId),
                                         std::move(destinationMailboxId));
    }

    javelin::jmap::QueuedEmailMutationResult
    LongPollCoordinator::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        return m_jmapCore.queueMarkEmailRead(std::move(accountId), std::move(emailId));
    }

    javelin::jmap::QueuedEmailMutationResult
    LongPollCoordinator::queueMarkEmailUnread(std::string accountId, std::string emailId)
    {
        return m_jmapCore.queueMarkEmailUnread(std::move(accountId), std::move(emailId));
    }

    javelin::jmap::QueuedEmailMutationResult
    LongPollCoordinator::queueSetEmailFlagged(std::string accountId, std::string emailId,
                                              const bool flagged)
    {
        return m_jmapCore.queueSetEmailFlagged(std::move(accountId), std::move(emailId), flagged);
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    LongPollCoordinator::submitPendingEmailMutations(std::string accountId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.submitPendingEmailMutations(configuration->second.settings,
                                                                  std::move(accountId));
    }

    QCoro::Task<javelin::jmap::MessageContentRefreshResult>
    LongPollCoordinator::requestMessageContent(std::string accountId, std::string emailId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.refreshMessageContent(
            configuration->second.settings, std::move(accountId), std::move(emailId));
    }

    QCoro::Task<javelin::jmap::AttachmentDownloadResult>
    LongPollCoordinator::requestAttachment(std::string accountId, std::string emailId,
                                           std::string partId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.downloadAttachment(configuration->second.settings,
                                                         std::move(accountId), std::move(emailId),
                                                         std::move(partId));
    }

    QCoro::Task<javelin::jmap::MessageSourceDownloadResult>
    LongPollCoordinator::requestMessageSource(std::string accountId, std::string emailId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_jmapCore.downloadMessageSource(
            configuration->second.settings, std::move(accountId), std::move(emailId));
    }

    QCoro::Task<javelin::jmap::LiveRefreshResult>
    LongPollCoordinator::bootstrapAccount(javelin::jmap::LiveConnectionSettings settings,
                                          std::vector<std::string> mailboxIds)
    {
        co_return co_await m_jmapCore.refreshFromServer(std::move(settings), {},
                                                        std::move(mailboxIds));
    }

    QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
    LongPollCoordinator::requestContacts(std::string accountId)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true,
            };
        co_return co_await m_contactService.refreshAll(configuration->second.settings,
                                                       std::move(accountId));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    LongPollCoordinator::setAddressBooks(std::string accountId,
                                         javelin::jmap::api::AddressBookSetRequest request)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.setAddressBooks(
            configuration->second.settings, std::move(accountId), std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    LongPollCoordinator::setContactCards(std::string accountId,
                                         javelin::jmap::api::ContactCardSetRequest request)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.setContactCards(
            configuration->second.settings, std::move(accountId), std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
    LongPollCoordinator::copyContactCards(std::string accountId,
                                          javelin::jmap::api::ContactCardCopyRequest request)
    {
        const auto configuration = m_configurations.find(accountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.copyContactCards(
            configuration->second.settings, std::move(accountId), std::move(request));
    }

    QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
    LongPollCoordinator::uploadContactMedia(std::string ownerAccountId, std::string accountId,
                                            QByteArray payload, std::string mediaType)
    {
        const auto configuration = m_configurations.find(ownerAccountId);
        if (configuration == m_configurations.end())
            co_return javelin::jmap::LiveRefreshError{
                .message = QStringLiteral("Account synchronization is not configured."),
                .requiresUserIntervention = true};
        co_return co_await m_contactService.uploadMedia(
            configuration->second.settings, std::move(ownerAccountId), std::move(accountId),
            std::move(payload), std::move(mediaType));
    }

    void LongPollCoordinator::stop()
    {
        for (const auto& [accountId, service] : m_services)
        {
            Q_EMIT accountStatusChanged(QString::fromStdString(accountId),
                                        LongPollService::Status::Disconnected);
            disconnect(service.get(), nullptr, this, nullptr);
        }
        m_services.clear();
    }

    void LongPollCoordinator::connectService(const std::string& accountId, LongPollService& service)
    {
        connect(&service, &LongPollService::statusChanged, this,
                [this, accountId](const auto status)
                { Q_EMIT accountStatusChanged(QString::fromStdString(accountId), status); });
        connect(&service, &LongPollService::cacheCommitted, this,
                &LongPollCoordinator::cacheCommitted);
        connect(&service, &LongPollService::notificationRaised, this,
                &LongPollCoordinator::notificationRaised);
    }

} // namespace javelin::app
