#include "app/LongPollCoordinator.h"

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
            auto [serviceIt, inserted] = m_services.try_emplace(configuration.accountId);
            if (inserted)
            {
                serviceIt->second = std::make_unique<LongPollService>(
                    m_databaseConnection, m_transport, m_networkAccessManager, m_accountRepository,
                    m_queryService, this);
                connectService(serviceIt->first, *serviceIt->second);
            }
            serviceIt->second->applySettings(std::move(configuration.settings),
                                             configuration.accountId,
                                             std::move(configuration.mailboxIds));
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
        connect(&service, &LongPollService::mailStateChanged, this,
                &LongPollCoordinator::mailStateChanged);
        connect(&service, &LongPollService::accountMailStateChanged, this,
                &LongPollCoordinator::accountMailStateChanged);
        connect(&service, &LongPollService::mailboxRefreshed, this,
                &LongPollCoordinator::mailboxRefreshed);
        connect(&service, &LongPollService::notificationRaised, this,
                &LongPollCoordinator::notificationRaised);
    }

} // namespace javelin::app
