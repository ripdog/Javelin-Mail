#pragma once

#include "app/CacheInvalidationPublisher.h"
#include "app/MailApplicationEventsPorts.h"

namespace javelin::app
{
    class AccountRuntimeManager;
    class ContactApplicationService;
    class MailMutationApplicationService;
    class MailQueryApplicationService;
    class MessageContentApplicationService;

    class MailApplicationEventsService final : public MailApplicationEventsPort
    {
        Q_OBJECT

      public:
        MailApplicationEventsService(AccountRuntimeManager& accountRuntime,
                                     MailQueryApplicationService& queries,
                                     MailMutationApplicationService& mutations,
                                     MessageContentApplicationService& content,
                                     ContactApplicationService& contacts,
                                     QObject* parent = nullptr);

        [[nodiscard]] std::unordered_map<std::string, MailAccountStatus>
        accountStatuses() const override;

      private:
        AccountRuntimeManager& m_accountRuntime;
        CacheInvalidationPublisher m_invalidationPublisher;
    };
} // namespace javelin::app
