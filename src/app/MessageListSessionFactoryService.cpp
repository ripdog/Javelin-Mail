#include "app/MessageListSessionFactoryService.h"

#include "app/MailApplicationEventsPorts.h"
#include "app/MailApplicationService.h"
#include "app/MailboxSession.h"
#include "app/SearchSession.h"

#include <utility>

namespace javelin::app
{
    MessageListSessionFactoryService::MessageListSessionFactoryService(
        MailApplicationService& service, MailApplicationEventsPort& events)
        : m_service(service), m_events(events)
    {
    }

    MailboxSession* MessageListSessionFactoryService::createMailboxSession(
        std::string accountId, std::string mailboxId, QString title,
        std::optional<std::string> role, javelin::jmap::query::EmailListSort sort,
        javelin::jmap::cache::QueryReader& queryReader, const std::size_t pageSize,
        std::optional<RestoredMailboxState> restored, QObject* parent)
    {
        return new MailboxSession(std::move(accountId), std::move(mailboxId), std::move(title),
                                  std::move(role), sort, queryReader, m_service, pageSize, m_events,
                                  std::move(restored), parent);
    }

    SearchSession* MessageListSessionFactoryService::createSearchSession(
        std::string accountId, javelin::jmap::search::EmailSearchCriteria criteria,
        javelin::jmap::query::EmailListSort sort, javelin::jmap::cache::QueryReader& queryReader,
        const std::size_t pageSize, std::optional<RestoredSearchState> restored, QObject* parent)
    {
        return new SearchSession(std::move(accountId), std::move(criteria), sort, queryReader,
                                 m_service, m_events, pageSize, std::move(restored), parent);
    }
} // namespace javelin::app
