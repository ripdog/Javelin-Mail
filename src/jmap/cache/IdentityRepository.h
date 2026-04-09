#pragma once

#include "jmap/cache/Database.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class IdentityRepository
    {
      public:
        explicit IdentityRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Identity>& identities);
        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Identity>, DatabaseError>
        listByAccount(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Identity>, DatabaseError>
        find(std::string_view accountId, std::string_view identityId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
