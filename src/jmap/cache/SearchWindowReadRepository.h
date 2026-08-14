#pragma once

#include "jmap/cache/SearchWindowRepository.h"
#include "storage/sqlite/DatabaseConnection.h"

namespace javelin::jmap::cache
{
    class SearchWindowReadRepository final
    {
      public:
        explicit SearchWindowReadRepository(DatabaseConnection& connection);
        explicit SearchWindowReadRepository(ReadOnlyDatabaseConnection& connection);
        explicit SearchWindowReadRepository(DatabaseReadView connection);

        [[nodiscard]] SearchWindowResult find(std::string_view accountId, std::string_view queryKey,
                                              std::size_t offset, std::size_t limit) const;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
