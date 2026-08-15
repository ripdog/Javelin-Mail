#pragma once

#include "storage/sqlite/DatabaseConnection.h"

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
        [[nodiscard]] std::variant<std::optional<CachedAccount>, DatabaseError>
        findById(std::string_view accountId) const override;
        [[nodiscard]] std::optional<DatabaseError> removeMany(const QStringList& accountIds);
        [[nodiscard]] std::optional<DatabaseError>
        claimLegacyConnection(std::string_view connectionId, const QStringList& knownAccountIds);
        [[nodiscard]] std::optional<DatabaseError>
        removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                const QStringList& knownAccountIds);

      private:
        DatabaseConnection& m_connection;
    };

} // namespace javelin::jmap::cache
