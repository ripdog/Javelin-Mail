#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    class EmailReadRepository final
    {
      public:
        explicit EmailReadRepository(const DatabaseReadView& connection);

        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Email>, DatabaseError>
        find(std::string_view accountId, std::string_view emailId) const;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
