#pragma once

#include "jmap/cache/Database.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/domain/MailEntities.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class IdentityRepository final : public IdentityReader
    {
      public:
        explicit IdentityRepository(DatabaseConnection& connection);
        explicit IdentityRepository(ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceAll(std::string_view accountId,
                   const std::vector<javelin::jmap::domain::Identity>& identities);
        [[nodiscard]] std::variant<std::vector<javelin::jmap::domain::Identity>, DatabaseError>
        listByAccount(std::string_view accountId) const override;
        [[nodiscard]] std::variant<std::optional<javelin::jmap::domain::Identity>, DatabaseError>
        find(std::string_view accountId, std::string_view identityId) const override;

      private:
        DatabaseReadView m_connection;
        DatabaseConnection* m_writeConnection = nullptr;
    };

} // namespace javelin::jmap::cache
