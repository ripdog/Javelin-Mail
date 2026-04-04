#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class MailboxRepository
    {
      public:
        explicit MailboxRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Mailbox>& mailboxes);
        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Mailbox>, DatabaseError>
        listByParent(std::string_view accountId, std::optional<std::string_view> parentId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
