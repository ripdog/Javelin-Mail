#pragma once

#include "jmap/cache/MessageListReadTypes.h"
#include "storage/DatabaseError.h"

#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class ThreadReader
    {
      public:
        virtual ~ThreadReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listThreadMessages(std::string_view accountId, std::string_view threadId) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxThreadMessages(std::string_view accountId, std::string_view mailboxId,
                                  std::string_view threadId,
                                  MailboxThreadMembershipSource membershipSource =
                                      MailboxThreadMembershipSource::NormalizedThread) const = 0;
    };

} // namespace javelin::jmap::cache
