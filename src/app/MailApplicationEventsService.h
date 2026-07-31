#pragma once

#include "app/CacheInvalidationPublisher.h"
#include "app/MailApplicationEventsPorts.h"

namespace javelin::app
{
    class MailApplicationService;

    class MailApplicationEventsService final : public MailApplicationEventsPort
    {
        Q_OBJECT

      public:
        explicit MailApplicationEventsService(MailApplicationService& service,
                                              QObject* parent = nullptr);

        [[nodiscard]] std::unordered_map<std::string, MailAccountStatus>
        accountStatuses() const override;

      private:
        MailApplicationService& m_service;
        CacheInvalidationPublisher m_invalidationPublisher;
    };
} // namespace javelin::app
