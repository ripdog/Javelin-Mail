#pragma once

#include "storage/DatabaseError.h"

#include <cstddef>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{
    class MailboxStatisticsReader
    {
      public:
        virtual ~MailboxStatisticsReader() = default;

        [[nodiscard]] virtual std::variant<std::size_t, DatabaseError>
        countMailboxMessages(std::string_view accountId, std::string_view mailboxId) const = 0;
        [[nodiscard]] virtual std::variant<std::size_t, DatabaseError>
        countUnreadMailboxEmails(std::string_view accountId, std::string_view mailboxId) const = 0;
    };

} // namespace javelin::jmap::cache
