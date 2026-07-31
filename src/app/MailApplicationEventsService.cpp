#include "app/MailApplicationEventsService.h"

#include "app/MailApplicationService.h"

#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] MailAccountStatus mapStatus(const AccountSyncCoordinator::Status status)
        {
            switch (status)
            {
            case AccountSyncCoordinator::Status::Disconnected:
                return MailAccountStatus::Disconnected;
            case AccountSyncCoordinator::Status::Connecting:
                return MailAccountStatus::Connecting;
            case AccountSyncCoordinator::Status::Connected:
                return MailAccountStatus::Connected;
            case AccountSyncCoordinator::Status::AuthenticationPaused:
                return MailAccountStatus::AuthenticationPaused;
            }
            return MailAccountStatus::Disconnected;
        }
    } // namespace

    MailApplicationEventsService::MailApplicationEventsService(MailApplicationService& service,
                                                               QObject* parent)
        : MailApplicationEventsPort(parent), m_service(service), m_invalidationPublisher(this)
    {
        connect(&m_service, &MailApplicationService::accountStatusChanged, this,
                [this](const QString& accountId, const AccountSyncCoordinator::Status status)
                { Q_EMIT accountStatusChanged(accountId, mapStatus(status)); });
        connect(&m_service, &MailApplicationService::sessionCapabilitiesChanged, this,
                &MailApplicationEventsPort::sessionCapabilitiesChanged);
        connect(&m_service, &MailApplicationService::cacheCommitted, &m_invalidationPublisher,
                [this](MailCacheChange change)
                { m_invalidationPublisher.publish(std::move(change)); });
        connect(&m_invalidationPublisher, &CacheInvalidationPublisher::invalidated, this,
                &MailApplicationEventsPort::cacheInvalidated);
    }

    std::unordered_map<std::string, MailAccountStatus>
    MailApplicationEventsService::accountStatuses() const
    {
        std::unordered_map<std::string, MailAccountStatus> result;
        for (const auto& [accountId, status] : m_service.accountStatuses())
            result.emplace(accountId, mapStatus(status));
        return result;
    }
} // namespace javelin::app
