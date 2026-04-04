#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class EmailRepository
    {
      public:
        explicit EmailRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Email>& emails);
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Email>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
