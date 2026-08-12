#pragma once

#include "storage/DatabaseError.h"

#include "jmap/cache/MessageListReadTypes.h"
#include "jmap/query/EmailListSort.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class MailboxMessageReader
    {
      public:
        virtual ~MailboxMessageReader() = default;

        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                            std::size_t limit, std::size_t offset = 0,
                            javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<OfflineMailboxCoverage>, DatabaseError>
        offlineMailboxCoverage(std::string_view accountId, std::string_view mailboxId) const = 0;
        [[nodiscard]] virtual std::variant<bool, DatabaseError>
        offlineMailboxComplete(std::string_view accountId, std::string_view mailboxId) const = 0;
        [[nodiscard]] virtual std::variant<std::optional<std::string>, DatabaseError>
        completeOfflineMailboxQueryState(std::string_view accountId, std::string_view mailboxId,
                                         std::string_view canonicalQueryKey) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<MessageListItem>, DatabaseError>
        listOfflineMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                   std::uint64_t generation, std::size_t limit,
                                   std::size_t offset = 0,
                                   javelin::jmap::query::EmailListSort sort = {}) const = 0;
        [[nodiscard]] virtual std::variant<std::vector<std::string>, DatabaseError>
        listOfflineMailboxRepresentativeIds(
            std::string_view accountId, std::string_view mailboxId, std::uint64_t generation,
            std::size_t limit, std::size_t offset = 0,
            javelin::jmap::query::EmailListSort sort = {}) const = 0;
    };

} // namespace javelin::jmap::cache
