#pragma once

#include "jmap/cache/Database.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct SearchWindowRecord
    {
        std::string accountId;
        std::string queryKey;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        bool isAuthoritative = true;
        std::vector<std::string> emailIds;
    };

    using SearchWindowResult = std::variant<std::optional<SearchWindowRecord>, DatabaseError>;

    class SearchWindowRepository
    {
      public:
        explicit SearchWindowRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError> replace(const SearchWindowRecord& window);
        [[nodiscard]] SearchWindowResult find(std::string_view accountId, std::string_view queryKey,
                                              std::size_t offset, std::size_t limit) const;
        [[nodiscard]] std::optional<DatabaseError>
        invalidateAccount(DatabaseTransaction& transaction, std::string_view accountId);
        [[nodiscard]] std::optional<DatabaseError> eraseQuery(std::string_view accountId,
                                                              std::string_view queryKey);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
