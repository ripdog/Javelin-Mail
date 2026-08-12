#pragma once

#include "jmap/api/Session.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    class SessionRepository
    {
      public:
        explicit SessionRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replace(std::string_view ownerAccountId, const javelin::jmap::api::Session& session);
        [[nodiscard]] std::variant<std::optional<javelin::jmap::api::Session>, DatabaseError>
        load(std::string_view ownerAccountId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
