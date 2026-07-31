#pragma once

#include "app/MessageListSession.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QObject>

#include <cstddef>
#include <optional>
#include <string>

namespace javelin::jmap::cache
{
    class QueryReader;
}

namespace javelin::app
{
    class MailApplicationService;
    class MailboxSession;
    class SearchSession;

    struct RestoredMailboxState
    {
        MessageListPage page;
    };

    enum class SearchMode
    {
        Local,
        Promoting,
        Online,
    };

    struct RestoredSearchState
    {
        MessageListPage page;
        SearchMode mode = SearchMode::Local;
        std::string sessionId;
    };

    class MessageListSessionFactoryPort
    {
      public:
        virtual ~MessageListSessionFactoryPort() = default;

        [[nodiscard]] virtual MailboxSession*
        createMailboxSession(std::string accountId, std::string mailboxId, QString title,
                             std::optional<std::string> role,
                             javelin::jmap::query::EmailListSort sort,
                             javelin::jmap::cache::QueryReader& queryReader, std::size_t pageSize,
                             std::optional<RestoredMailboxState> restored, QObject* parent) = 0;
        [[nodiscard]] virtual SearchSession*
        createSearchSession(std::string accountId,
                            javelin::jmap::search::EmailSearchCriteria criteria,
                            javelin::jmap::query::EmailListSort sort,
                            javelin::jmap::cache::QueryReader& queryReader, std::size_t pageSize,
                            std::optional<RestoredSearchState> restored, QObject* parent) = 0;
    };
} // namespace javelin::app
