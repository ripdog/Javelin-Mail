#include "app/MessageListSessionFactoryService.h"

#include "app/MailApplicationEventsPorts.h"
#include "app/MailboxSession.h"
#include "app/SearchSession.h"

#include <utility>

namespace javelin::app
{
    MessageListSessionFactoryService::MessageListSessionFactoryService(
        MessageListMaterializationPort& materializationPort, MailApplicationEventsPort& events,
        QString databasePath)
        : m_materializationPort(materializationPort), m_events(events),
          m_databasePath(std::move(databasePath))
    {
    }

    MailboxSession* MessageListSessionFactoryService::createMailboxSession(
        std::string accountId, std::string mailboxId, QString title,
        std::optional<std::string> role, javelin::jmap::query::EmailListSort sort,
        const std::size_t windowSize, std::optional<RestoredMailboxState> restored, QObject* parent)
    {
        return new MailboxSession(std::move(accountId), std::move(mailboxId), std::move(title),
                                  std::move(role), sort, m_databasePath, m_materializationPort,
                                  windowSize, m_events, std::move(restored), parent);
    }

    SearchSession* MessageListSessionFactoryService::createSearchSession(
        std::string accountId, javelin::jmap::search::EmailSearchCriteria criteria,
        javelin::jmap::query::EmailListSort sort, const std::size_t windowSize,
        std::optional<RestoredSearchState> restored, QObject* parent)
    {
        return new SearchSession(std::move(accountId), std::move(criteria), sort, m_databasePath,
                                 m_materializationPort, m_events, windowSize, std::move(restored),
                                 parent);
    }
} // namespace javelin::app
