#pragma once

#include "jmap/cache/MessageListReadTypes.h"
#include "jmap/query/EmailListSort.h"
#include "storage/DatabaseError.h"

#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class MailSearchReader
    {
      public:
        virtual ~MailSearchReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        searchCachedMessageText(std::string_view accountId, std::string_view text,
                                std::size_t limit, std::size_t offset = 0) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        searchAllCachedMessageText(std::string_view accountId, std::string_view text,
                                   javelin::jmap::query::EmailListSort sort = {}) const = 0;
    };

} // namespace javelin::jmap::cache
