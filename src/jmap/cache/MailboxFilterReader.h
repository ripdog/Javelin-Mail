#pragma once

#include "storage/DatabaseError.h"

#include "jmap/cache/MessageListReadTypes.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"

#include <cstddef>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class MailboxFilterReader
    {
      public:
        virtual ~MailboxFilterReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listFilteredMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                    const javelin::jmap::search::EmailSearchCriteria& criteria,
                                    std::size_t limit, std::size_t offset = 0,
                                    javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::size_t, DatabaseError> countFilteredMailboxMessages(
            std::string_view accountId, std::string_view mailboxId,
            const javelin::jmap::search::EmailSearchCriteria& criteria) const = 0;
    };

} // namespace javelin::jmap::cache
