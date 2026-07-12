#include "app/LongPollCoordinator.h"

#include <ranges>
#include <unordered_set>

namespace javelin::app
{

    LongPollCoordinator::LongPollCoordinator(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        javelin::jmap::api::AbstractTransport& transport,
        QNetworkAccessManager& networkAccessManager,
        javelin::jmap::cache::AccountRepository& accountRepository,
        javelin::jmap::cache::QueryService& queryService, QObject* parent)
        : QObject(parent), m_databaseConnection(databaseConnection), m_transport(transport),
          m_networkAccessManager(networkAccessManager), m_accountRepository(accountRepository),
          m_queryService(queryService)
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
