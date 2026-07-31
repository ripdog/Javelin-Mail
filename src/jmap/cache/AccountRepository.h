#pragma once

#include "jmap/cache/AccountReadRepository.h"

#include <QStringList>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{

    class AccountRepository final : public AccountReader
    {
      public:
        explicit AccountRepository(DatabaseConnection& connection);

        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError>
        listAll() const override;
        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError>
        listOwnedBy(std::string_view ownerAccountId) const override;
        [[nodiscard]] std::optional<DatabaseError> removeMany(const QStringList& accountIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                const QStringList& knownAccountIds);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
