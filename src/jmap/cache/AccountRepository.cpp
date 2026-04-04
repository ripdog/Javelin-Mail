#include "jmap/cache/AccountRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + ": " + query.lastError().text(),
            };
        }

    } // namespace

    AccountRepository::AccountRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::variant<std::vector<CachedAccount>, DatabaseError> AccountRepository::listAll() const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        if (!query.exec("SELECT account_id, name, is_personal, is_read_only, is_primary "
                        "FROM accounts "
                        "ORDER BY is_primary DESC, name, account_id"))
        {
            return makeQueryError("Read cached accounts", query);
        }

        std::vector<CachedAccount> accounts;
        while (query.next())
        {
            accounts.push_back(CachedAccount{
                .accountId = query.value(0).toString().toStdString(),
                .name = query.value(1).toString().toStdString(),
                .isPersonal = query.value(2).toInt() != 0,
                .isReadOnly = query.value(3).toInt() != 0,
                .isPrimary = query.value(4).toInt() != 0,
            });
        }

        return accounts;
    }

} // namespace javelin::jmap::cache
