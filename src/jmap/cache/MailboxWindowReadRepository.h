#pragma once

#include "jmap/cache/MailboxWindowRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class MailboxWindowReadRepository final
    {
      public:
        explicit MailboxWindowReadRepository(DatabaseConnection& connection);
        explicit MailboxWindowReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit MailboxWindowReadRepository(DatabaseReadView connection);

        [[nodiscard]] MailboxWindowResult find(std::string_view accountId,
                                               std::string_view queryKey,
                                               std::size_t requestedOffset,
                                               std::size_t requestedLimit) const;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
