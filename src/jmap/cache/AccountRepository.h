#pragma once

#include "jmap/cache/Database.h"

#include <string>
#include <QStringList>
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
        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError>
        listOwnedBy(std::string_view ownerAccountId) const;
        [[nodiscard]] std::optional<DatabaseError> removeMany(const QStringList& accountIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                const QStringList& knownAccountIds);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
