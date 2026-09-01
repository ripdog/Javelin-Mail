#include "app/MailApplicationEventsService.h"

#include "app/AccountRuntimeManager.h"
#include "app/ContactApplicationService.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailQueryApplicationService.h"
#include "app/MessageContentApplicationService.h"

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

    MailApplicationEventsService::MailApplicationEventsService(
        AccountRuntimeManager& accountRuntime, MailQueryApplicationService& queries,
        MailMutationApplicationService& mutations, MessageContentApplicationService& content,
        ContactApplicationService& contacts, QObject* parent)
        : MailApplicationEventsPort(parent), m_accountRuntime(accountRuntime),
          m_invalidationPublisher(this)
    {
        connect(&m_accountRuntime, &AccountRuntimeManager::accountStatusChanged, this,
                [this](const QString& accountId, const AccountSyncCoordinator::Status status)
                { Q_EMIT accountStatusChanged(accountId, mapStatus(status)); });
        connect(&m_accountRuntime, &AccountRuntimeManager::sessionCapabilitiesChanged, this,
                &MailApplicationEventsPort::sessionCapabilitiesChanged);

        const auto publish = [this](MailCacheChange change)
        { m_invalidationPublisher.publish(std::move(change)); };
        connect(&m_accountRuntime, &AccountRuntimeManager::cacheCommitted, &m_invalidationPublisher,
                publish);
        connect(&queries, &MailQueryApplicationService::cacheCommitted, &m_invalidationPublisher,
                publish);
        connect(&mutations, &MailMutationApplicationService::cacheCommitted,
                &m_invalidationPublisher, [this](MailCacheChange change)
                { m_invalidationPublisher.publishImmediately(std::move(change)); });
        connect(&content, &MessageContentApplicationService::cacheCommitted,
                &m_invalidationPublisher, publish);
        connect(&contacts, &ContactApplicationService::cacheCommitted, &m_invalidationPublisher,
                publish);

        connect(&m_invalidationPublisher, &CacheInvalidationPublisher::invalidated, this,
                &MailApplicationEventsPort::cacheInvalidated);
        connect(&queries, &MailQueryApplicationService::threadMaterializationProgress, this,
                &MailApplicationEventsPort::threadMaterializationProgress);
    }

    std::unordered_map<std::string, MailAccountStatus>
    MailApplicationEventsService::accountStatuses() const
    {
        std::unordered_map<std::string, MailAccountStatus> result;
        for (const auto& [accountId, status] : m_accountRuntime.accountStatuses())
            result.emplace(accountId, mapStatus(status));
        return result;
    }
} // namespace javelin::app
