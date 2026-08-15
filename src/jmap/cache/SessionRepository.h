#pragma once

#include "jmap/api/Session.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace javelin::jmap::cache
{
    struct StoredSessionAccounts
    {
        std::string ownerAccountId;
        std::unordered_map<std::string, std::string> accountIdsByRemoteId;
    };

    using SessionReplaceResult = std::variant<StoredSessionAccounts, DatabaseError>;

    class SessionRepository
    {
      public:
        explicit SessionRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replace(std::string_view ownerAccountId, const javelin::jmap::api::Session& session);
        [[nodiscard]] SessionReplaceResult
        replaceForConnection(std::string_view connectionId, std::string_view ownerRemoteAccountId,
                             const javelin::jmap::api::Session& session);
        [[nodiscard]] std::variant<std::optional<javelin::jmap::api::Session>, DatabaseError>
        load(std::string_view ownerAccountId) const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
