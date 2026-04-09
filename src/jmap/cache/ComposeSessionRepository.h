#pragma once

#include "jmap/cache/Database.h"
#include "jmap/submission/ComposeTypes.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class ComposeSessionRepository
    {
      public:
        explicit ComposeSessionRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        upsert(const javelin::jmap::submission::DraftSnapshot& snapshot);
        [[nodiscard]]
        std::variant<std::optional<javelin::jmap::submission::DraftSnapshot>, DatabaseError>
        find(std::string_view composeSessionId) const;
        [[nodiscard]]
        std::variant<std::vector<javelin::jmap::submission::DraftSnapshot>, DatabaseError>
        listByAccount(std::string_view accountId) const;
        [[nodiscard]] std::optional<DatabaseError> remove(std::string_view composeSessionId);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
