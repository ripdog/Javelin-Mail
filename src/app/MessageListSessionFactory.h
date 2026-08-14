#pragma once

#include "app/MessageListSession.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <QObject>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace javelin::app
{
    class MailboxSession;
    class SearchSession;

    enum class SearchMode
    {
        Local,
        Promoting,
        Online,
    };

    struct RestoredMailboxState
    {
        std::vector<MessageListWindowRequest> windows;
    };

    struct RestoredSearchState
    {
        SearchMode mode = SearchMode::Local;
        std::string sessionId;
        std::vector<MessageListWindowRequest> windows;
    };

    class MessageListSessionFactoryPort
    {
      public:
        virtual ~MessageListSessionFactoryPort() = default;

        [[nodiscard]] virtual MailboxSession*
        createMailboxSession(std::string accountId, std::string mailboxId, QString title,
                             std::optional<std::string> role,
                             javelin::jmap::query::EmailListSort sort, std::size_t windowSize,
                             std::optional<RestoredMailboxState> restored, QObject* parent) = 0;
        [[nodiscard]] virtual SearchSession*
        createSearchSession(std::string accountId,
                            javelin::jmap::search::EmailSearchCriteria criteria,
                            javelin::jmap::query::EmailListSort sort, std::size_t windowSize,
                            std::optional<RestoredSearchState> restored, QObject* parent) = 0;
    };
} // namespace javelin::app
