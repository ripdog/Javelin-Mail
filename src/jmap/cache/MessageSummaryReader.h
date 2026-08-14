#pragma once

#include "jmap/cache/MessageListReadTypes.h"
#include "storage/DatabaseError.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class MessageSummaryReader
    {
      public:
        virtual ~MessageSummaryReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listMessagesByEmailIds(std::string_view accountId,
                               const std::vector<std::string>& emailIds) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<MessageListItem>, DatabaseError>
        findMailboxMessage(std::string_view accountId, std::string_view mailboxId,
                           std::string_view emailId) const = 0;
    };

} // namespace javelin::jmap::cache
