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
            auto [serviceIt, inserted] = m_services.try_emplace(configuration.accountId);
            if (inserted)
            {
                serviceIt->second = std::make_unique<LongPollService>(
                    m_databaseConnection, m_transport, m_networkAccessManager, m_accountRepository,
                    m_queryService, this);
                connectService(*serviceIt->second);
            }
            serviceIt->second->applySettings(std::move(configuration.settings),
                                             configuration.accountId);
        }

        for (auto serviceIt = m_services.begin(); serviceIt != m_services.end();)
        {
            if (configuredAccountIds.contains(serviceIt->first))
            {
                ++serviceIt;
                continue;
            }

            disconnect(serviceIt->second.get(), nullptr, this, nullptr);
            serviceIt = m_services.erase(serviceIt);
        }
        updateStatus();
    }

    void LongPollCoordinator::stop()
    {
        for (const auto& service : m_services | std::views::values)
        {
            disconnect(service.get(), nullptr, this, nullptr);
        }
        m_services.clear();
        updateStatus();
    }

    LongPollService::Status LongPollCoordinator::status() const
    {
        return m_status;
    }

    void LongPollCoordinator::connectService(LongPollService& service)
    {
        connect(&service, &LongPollService::statusChanged, this,
                [this](const auto) { updateStatus(); });
        connect(&service, &LongPollService::mailStateChanged, this,
                &LongPollCoordinator::mailStateChanged);
        connect(&service, &LongPollService::accountMailStateChanged, this,
                &LongPollCoordinator::accountMailStateChanged);
        connect(&service, &LongPollService::mailboxRefreshed, this,
                &LongPollCoordinator::mailboxRefreshed);
        connect(&service, &LongPollService::notificationRaised, this,
                &LongPollCoordinator::notificationRaised);
    }

    void LongPollCoordinator::updateStatus()
    {
        auto nextStatus = LongPollService::Status::Disconnected;
        if (std::ranges::any_of(m_services,
                                [](const auto& entry)
                                {
                                    return entry.second != nullptr &&
                                           entry.second->status() ==
                                               LongPollService::Status::Connecting;
                                }))
        {
            nextStatus = LongPollService::Status::Connecting;
        }
        if (!m_services.empty() &&
            std::ranges::all_of(m_services,
                                [](const auto& entry)
                                {
                                    return entry.second != nullptr &&
                                           entry.second->status() ==
                                               LongPollService::Status::Connected;
                                }))
        {
            nextStatus = LongPollService::Status::Connected;
        }
        if (m_status != nextStatus)
        {
            m_status = nextStatus;
            Q_EMIT statusChanged(m_status);
        }
    }

} // namespace javelin::app
