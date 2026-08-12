#pragma once

#include "jmap/cache/ThreadReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class ThreadReadRepository final : public ThreadReader
    {
      public:
        explicit ThreadReadRepository(DatabaseConnection& connection);
        explicit ThreadReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit ThreadReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listThreadMessages(std::string_view accountId, std::string_view threadId) const override;
        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listMailboxThreadMessages(
            std::string_view accountId, std::string_view mailboxId, std::string_view threadId,
            MailboxThreadMembershipSource membershipSource =
                MailboxThreadMembershipSource::NormalizedThread) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
