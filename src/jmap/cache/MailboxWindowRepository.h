#pragma once

#include "jmap/cache/Database.h"
#include "jmap/cache/QueryWindowCoverage.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct MailboxWindowRecord
    {
        std::string accountId;
        std::string mailboxId;
        std::string queryKey;
        std::size_t requestedOffset = 0;
        std::size_t requestedLimit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        QueryWindowCoverage coverage = QueryWindowCoverage::Server;
        std::vector<std::string> emailIds;
    };

    struct MailboxWindowAddition
    {
        std::string emailId;
        std::size_t index = 0;
    };

    using MailboxWindowResult = std::variant<std::optional<MailboxWindowRecord>, DatabaseError>;

    class MailboxWindowRepository
    {
      public:
        explicit MailboxWindowRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError> replace(const MailboxWindowRecord& window);
        [[nodiscard]] MailboxWindowResult find(std::string_view accountId,
                                               std::string_view queryKey,
                                               std::size_t requestedOffset,
                                               std::size_t requestedLimit) const;
        [[nodiscard]] std::optional<DatabaseError> invalidateMailbox(std::string_view accountId,
                                                                     std::string_view mailboxId);
        [[nodiscard]] std::optional<DatabaseError>
        invalidateMailbox(DatabaseTransaction& transaction, std::string_view accountId,
                          std::string_view mailboxId,
                          QueryWindowCoverage coverage = QueryWindowCoverage::Stale);
        [[nodiscard]] std::optional<DatabaseError>
        rebaseContiguousPrefix(DatabaseTransaction& transaction, std::string_view accountId,
                               std::string_view mailboxId, std::string_view queryKey,
                               std::string_view sinceQueryState, std::string_view newQueryState,
                               std::vector<MailboxWindowAddition> additions,
                               std::vector<std::string> removals, std::optional<std::size_t> total);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
