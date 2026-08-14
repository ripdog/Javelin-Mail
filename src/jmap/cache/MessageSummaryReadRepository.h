#pragma once

#include "jmap/cache/MessageSummaryReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class MessageSummaryReadRepository final : public MessageSummaryReader
    {
      public:
        explicit MessageSummaryReadRepository(DatabaseConnection& connection);
        explicit MessageSummaryReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MessageSummaryReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMessagesByEmailIds(std::string_view accountId,
                               const std::vector<std::string>& emailIds) const override;
        [[nodiscard]] std::variant<std::optional<MessageListItem>, DatabaseError>
        findMailboxMessage(std::string_view accountId, std::string_view mailboxId,
                           std::string_view emailId) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
