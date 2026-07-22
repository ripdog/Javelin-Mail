#include "jmap/cache/AccountRepository.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>

#include <QSet>

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
        if (!query.exec(
                QStringLiteral("SELECT account_id, name, is_personal, is_read_only, is_primary "
                               "FROM accounts "
                               "ORDER BY is_primary DESC, name, account_id")))
        {
            return makeQueryError(QStringLiteral("Read cached accounts"), query);
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

    std::variant<std::vector<CachedAccount>, DatabaseError>
    AccountRepository::listOwnedBy(const std::string_view ownerAccountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT account_id, name, is_personal, is_read_only, is_primary FROM accounts "
            "WHERE owner_account_id = :owner_account_id ORDER BY is_primary DESC, name, "
            "account_id"));
        query.bindValue(QStringLiteral(":owner_account_id"),
                        QString::fromStdString(std::string{ownerAccountId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read session-owned accounts"), query);
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

    std::optional<DatabaseError> AccountRepository::removeMany(const QStringList& accountIds)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }
        if (accountIds.empty())
        {
            return std::nullopt;
        }

        const DatabaseWriteScope writeScope{m_connection};
        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin account removal transaction: ") +
                                            database.lastError().text()};
        }

        QSqlQuery query{database};
        query.prepare(QStringLiteral("DELETE FROM accounts WHERE account_id = :account_id"));
        for (const auto& accountId : accountIds)
        {
            query.bindValue(QStringLiteral(":account_id"), accountId);
            if (!query.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Remove cached account"), query);
            }
        }
        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit account removal: ") +
                                            database.lastError().text()};
        }
        return std::nullopt;
    }

    std::optional<DatabaseError>
    AccountRepository::removeConfiguredAccount(const QString& loginEmail, const QString& sessionUrl,
                                               const QStringList& knownAccountIds)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSet<QString> ownerAccountIds{knownAccountIds.begin(), knownAccountIds.end()};
        const QUrl configuredUrl{sessionUrl};
        QSqlQuery sessions{m_connection.database()};
        sessions.prepare(
            QStringLiteral("SELECT account_id, api_url FROM sessions WHERE username = :username"));
        sessions.bindValue(QStringLiteral(":username"), loginEmail);
        if (!sessions.exec())
        {
            return makeQueryError(QStringLiteral("Resolve configured account sessions"), sessions);
        }
        while (sessions.next())
        {
            const QUrl apiUrl{sessions.value(1).toString()};
            const bool sameServer =
                configuredUrl.isValid() && apiUrl.isValid() &&
                configuredUrl.scheme().compare(apiUrl.scheme(), Qt::CaseInsensitive) == 0 &&
                configuredUrl.host().compare(apiUrl.host(), Qt::CaseInsensitive) == 0 &&
                configuredUrl.port() == apiUrl.port();
            if (sameServer)
            {
                ownerAccountIds.insert(sessions.value(0).toString());
            }
        }

        if (ownerAccountIds.empty())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "No cached account data matched this configured login. Nothing was removed."),
            };
        }

        const DatabaseWriteScope writeScope{m_connection};
        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin account removal transaction: ") +
                                            database.lastError().text()};
        }

        QSqlQuery deleteOwnedAccounts{database};
        deleteOwnedAccounts.prepare(
            QStringLiteral("DELETE FROM accounts WHERE owner_account_id = :owner_account_id "
                           "OR account_id = :owner_account_id"));
        for (const auto& ownerAccountId : ownerAccountIds)
        {
            deleteOwnedAccounts.bindValue(QStringLiteral(":owner_account_id"), ownerAccountId);
            if (!deleteOwnedAccounts.exec())
            {
                database.rollback();
                return makeQueryError(QStringLiteral("Remove configured account cache"),
                                      deleteOwnedAccounts);
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit account removal: ") +
                                            database.lastError().text()};
        }
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
