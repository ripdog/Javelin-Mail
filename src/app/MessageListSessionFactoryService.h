#pragma once

#include "app/MessageListSessionFactory.h"

namespace javelin::app
{
    class MailApplicationService;
    class MailApplicationEventsPort;

    class MessageListSessionFactoryService final : public MessageListSessionFactoryPort
    {
      public:
        MessageListSessionFactoryService(MailApplicationService& service,
                                         MailApplicationEventsPort& events);

        [[nodiscard]] MailboxSession* createMailboxSession(
            std::string accountId, std::string mailboxId, QString title,
            std::optional<std::string> role, javelin::jmap::query::EmailListSort sort,
            javelin::jmap::cache::QueryReader& queryReader, std::size_t pageSize,
            std::optional<RestoredMailboxState> restored, QObject* parent) override;
        [[nodiscard]] SearchSession*
        createSearchSession(std::string accountId,
                            javelin::jmap::search::EmailSearchCriteria criteria,
                            javelin::jmap::query::EmailListSort sort,
                            javelin::jmap::cache::QueryReader& queryReader, std::size_t pageSize,
                            std::optional<RestoredSearchState> restored, QObject* parent) override;

      private:
        MailApplicationService& m_service;
        MailApplicationEventsPort& m_events;
    };
} // namespace javelin::app
