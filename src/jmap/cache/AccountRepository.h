#pragma once

#include "jmap/cache/Database.h"

#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    struct CachedAccount
    {
        std::string accountId;
        std::string name;
        bool isPersonal = false;
        bool isReadOnly = false;
        bool isPrimary = false;
    };

    class AccountRepository
    {
      public:
        explicit AccountRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError> listAll() const;

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
