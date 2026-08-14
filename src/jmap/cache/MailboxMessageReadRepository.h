#pragma once

#include "jmap/cache/MailboxMessageReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{

    class MailboxMessageReadRepository final : public MailboxMessageReader
    {
      public:
        explicit MailboxMessageReadRepository(DatabaseConnection& connection);
        explicit MailboxMessageReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MailboxMessageReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                            std::size_t limit, std::size_t offset = 0,
                            javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] std::variant<std::optional<OfflineMailboxCoverage>, DatabaseError>
        offlineMailboxCoverage(std::string_view accountId,
                               std::string_view mailboxId) const override;
        [[nodiscard]] std::variant<bool, DatabaseError>
        offlineMailboxComplete(std::string_view accountId,
                               std::string_view mailboxId) const override;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        completeOfflineMailboxQueryState(std::string_view accountId, std::string_view mailboxId,
                                         std::string_view canonicalQueryKey) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listOfflineMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                   std::uint64_t generation, std::size_t limit,
                                   std::size_t offset = 0,
                                   javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        listOfflineMailboxRepresentativeIds(
            std::string_view accountId, std::string_view mailboxId, std::uint64_t generation,
            std::size_t limit, std::size_t offset = 0,
            javelin::jmap::query::EmailListSort sort = {}) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
