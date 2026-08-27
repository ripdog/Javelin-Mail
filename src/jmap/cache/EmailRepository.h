#pragma once

#include "jmap/domain/MailEntities.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct SearchIndexUpdate
    {
        std::string emailId;
        std::string contentHash;
        std::string preview;
    };

    class EmailRepository
    {
      public:
        explicit EmailRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Email>& emails);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Email>& emails);
        [[nodiscard]] std::optional<DatabaseError>
        upsertMany(DatabaseTransaction& transaction, std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Email>& emails);
        [[nodiscard]] std::optional<DatabaseError>
        removeMany(std::string_view accountId, std::span<const std::string> emailIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeMany(DatabaseTransaction& transaction, std::string_view accountId,
                   std::span<const std::string> emailIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeFromMailbox(std::string_view accountId, std::string_view mailboxId,
                          std::span<const std::string> emailIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeFromMailbox(DatabaseTransaction& transaction, std::string_view accountId,
                          std::string_view mailboxId, std::span<const std::string> emailIds);
        [[nodiscard]] std::optional<DatabaseError> markSearchIndexed(std::string_view accountId,
                                                                     std::string_view emailId,
                                                                     std::string_view contentHash,
                                                                     std::string_view preview);
        [[nodiscard]] std::optional<DatabaseError>
        markSearchIndexedMany(std::string_view accountId,
                              std::span<const SearchIndexUpdate> updates);
        [[nodiscard]] std::variant<bool, DatabaseError> hasAny(std::string_view accountId) const;
        [[nodiscard]] std::variant<bool, DatabaseError> hasAny(DatabaseTransaction& transaction,
                                                               std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        existingIds(std::string_view accountId, std::span<const std::string> emailIds) const;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listMailboxEmailIds(std::string_view accountId, std::string_view mailboxId) const;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Email>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
