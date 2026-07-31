#include "jmap/cache/AccountReadRepository.h"

#include <QSqlError>
#include <QSqlQuery>

#include <optional>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] CachedAccount readAccount(const QSqlQuery& query)
        {
            return CachedAccount{
                .accountId = query.value(0).toString().toStdString(),
                .name = query.value(1).toString().toStdString(),
                .isPersonal = query.value(2).toInt() != 0,
                .isReadOnly = query.value(3).toInt() != 0,
                .isPrimary = query.value(4).toInt() != 0,
            };
        }

        [[nodiscard]] std::variant<std::vector<CachedAccount>, DatabaseError>
        readAccounts(const QSqlDatabase& database, const QString& operation,
                     const std::optional<QString>& ownerAccountId)
        {
            QSqlQuery query{database};
            if (ownerAccountId.has_value())
            {
                query.prepare(
                    QStringLiteral("SELECT account_id, name, is_personal, is_read_only, is_primary "
                                   "FROM accounts WHERE owner_account_id = :owner_account_id "
                                   "ORDER BY is_primary DESC, name, account_id"));
                query.bindValue(QStringLiteral(":owner_account_id"), *ownerAccountId);
            }
            else
            {
                query.prepare(
                    QStringLiteral("SELECT account_id, name, is_personal, is_read_only, is_primary "
                                   "FROM accounts ORDER BY is_primary DESC, name, account_id"));
            }
            if (!query.exec())
                return makeQueryError(operation, query);

            std::vector<CachedAccount> accounts;
            while (query.next())
                accounts.push_back(readAccount(query));
            return accounts;
        }

    } // namespace

    AccountReadRepository::AccountReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<CachedAccount>, DatabaseError> AccountReadRepository::listAll() const
    {
        if (const auto error = m_connection.validate())
            return *error;
        return readAccounts(m_connection.database(), QStringLiteral("Read cached accounts"),
                            std::nullopt);
    }

    std::variant<std::vector<CachedAccount>, DatabaseError>
    AccountReadRepository::listOwnedBy(const std::string_view ownerAccountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        return readAccounts(m_connection.database(), QStringLiteral("Read session-owned accounts"),
                            QString::fromStdString(std::string{ownerAccountId}));
    }

} // namespace javelin::jmap::cache
