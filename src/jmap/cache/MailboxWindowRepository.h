#pragma once

#include "jmap/cache/Database.h"

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
        std::vector<std::string> emailIds;
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
                          std::string_view mailboxId);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
