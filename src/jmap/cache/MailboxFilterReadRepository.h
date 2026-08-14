#pragma once

#include "jmap/cache/MailboxFilterReader.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{

    class MailboxFilterReadRepository final : public MailboxFilterReader
    {
      public:
        explicit MailboxFilterReadRepository(DatabaseConnection& connection);
        explicit MailboxFilterReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MailboxFilterReadRepository(DatabaseReadView connection);

        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        listFilteredMailboxMessages(std::string_view accountId, std::string_view mailboxId,
                                    const javelin::jmap::search::EmailSearchCriteria& criteria,
                                    std::size_t limit, std::size_t offset = 0,
                                    javelin::jmap::query::EmailListSort sort = {}) const override;
        [[nodiscard]] std::variant<std::size_t, DatabaseError> countFilteredMailboxMessages(
            std::string_view accountId, std::string_view mailboxId,
            const javelin::jmap::search::EmailSearchCriteria& criteria) const override;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
