#pragma once

#include "jmap/cache/MailSearchReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class MailSearchReadRepository final : public MailSearchReader
    {
      public:
        explicit MailSearchReadRepository(DatabaseConnection& connection);
        explicit MailSearchReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MailSearchReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        searchCachedMessageText(std::string_view accountId, std::string_view text,
                                std::size_t limit, std::size_t offset = 0) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        searchAllCachedMessageText(std::string_view accountId, std::string_view text,
                                   javelin::jmap::query::EmailListSort sort = {}) const override;

      private:
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listSortedMessages(std::string_view accountId, const std::vector<std::string>& emailIds,
                           javelin::jmap::query::EmailListSort sort) const;

        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
