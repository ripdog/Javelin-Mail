#pragma once

#include "jmap/cache/Database.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::cache
{

    struct SyncStateKey
    {
        std::string accountId;
        std::string objectType;
        std::string queryKey;
    };

    struct SyncStateRecord
    {
        SyncStateKey key;
        std::string stateToken;
        std::string updatedAt;
    };

    class SyncStateRepository
    {
      public:
        explicit SyncStateRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError> upsert(const SyncStateKey& key,
                                                          std::string_view stateToken);
        [[nodiscard]] std::optional<DatabaseError> upsert(DatabaseTransaction& transaction,
                                                          const SyncStateKey& key,
                                                          std::string_view stateToken);
        [[nodiscard]] std::variant<bool, DatabaseError>
        advanceIfCurrent(DatabaseTransaction& transaction, const SyncStateKey& key,
                         std::string_view expectedState, std::string_view newState);
        [[nodiscard]] std::variant<std::optional<SyncStateRecord>, DatabaseError>
        find(const SyncStateKey& key) const;
        [[nodiscard]] std::optional<DatabaseError> remove(const SyncStateKey& key);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
