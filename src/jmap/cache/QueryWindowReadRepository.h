#pragma once

#include "jmap/cache/QueryWindowReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class MailboxMessageReader;

    class QueryWindowReadRepository final : public QueryWindowReader
    {
      public:
        QueryWindowReadRepository(DatabaseConnection& connection,
                                  const MailboxMessageReader& mailboxReader);
        QueryWindowReadRepository(ReadOnlyDatabaseConnection& connection,
                                  const MailboxMessageReader& mailboxReader);
        QueryWindowReadRepository(DatabaseReadView connection,
                                  const MailboxMessageReader& mailboxReader);

        [[nodiscard]] std::variant<std::optional<SearchWindowPage>, DatabaseError>
        loadSearchWindow(std::string_view accountId, std::string_view queryKey, std::size_t offset,
                         std::size_t limit) const override;
        [[nodiscard]] std::variant<std::optional<MailboxWindowPage>, DatabaseError>
        loadMailboxWindow(std::string_view accountId, std::string_view queryKey,
                          std::size_t requestedOffset, std::size_t requestedLimit,
                          javelin::jmap::query::EmailListSort sort = {}) const override;

      private:
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxWindowMessagesByEmailIds(std::string_view accountId, std::string_view mailboxId,
                                            const std::vector<std::string>& emailIds) const;

        DatabaseReadView m_connection;
        const MailboxMessageReader& m_mailboxReader;
    };

} // namespace javelin::jmap::cache
