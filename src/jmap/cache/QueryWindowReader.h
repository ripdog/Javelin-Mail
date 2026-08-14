#pragma once

#include "jmap/cache/MessageListReadTypes.h"
#include "jmap/query/EmailListSort.h"
#include "storage/DatabaseError.h"

#include <optional>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{
    class QueryWindowReader
    {
      public:
        virtual ~QueryWindowReader() = default;

        [[nodiscard]] virtual std::variant<std::optional<SearchWindowPage>, DatabaseError>
        loadSearchWindow(std::string_view accountId, std::string_view queryKey, std::size_t offset,
                         std::size_t limit) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<MailboxWindowPage>, DatabaseError>
        loadMailboxWindow(std::string_view accountId, std::string_view queryKey,
                          std::size_t requestedOffset, std::size_t requestedLimit,
                          javelin::jmap::query::EmailListSort sort = {}) const = 0;
    };

} // namespace javelin::jmap::cache
