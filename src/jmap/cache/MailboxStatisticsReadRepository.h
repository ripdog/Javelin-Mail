#pragma once

#include "jmap/cache/MailboxStatisticsReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class MailboxStatisticsReadRepository final : public MailboxStatisticsReader
    {
      public:
        explicit MailboxStatisticsReadRepository(DatabaseConnection& connection);
        explicit MailboxStatisticsReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MailboxStatisticsReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countMailboxMessages(std::string_view accountId, std::string_view mailboxId) const override;
        [[nodiscard]] std::variant<std::size_t, DatabaseError>
        countUnreadMailboxEmails(std::string_view accountId,
                                 std::string_view mailboxId) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
