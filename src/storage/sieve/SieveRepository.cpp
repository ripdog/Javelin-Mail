#include "jmap/cache/SieveRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return {
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] std::optional<DatabaseError>
        writeScripts(DatabaseConnection& connection, const std::string_view accountId,
                     const std::vector<javelin::jmap::sieve::SieveScript>& scripts)
        {
            auto& database = connection.database();
            QSqlQuery clear{database};
            clear.prepare(QStringLiteral("DELETE FROM sieve_scripts WHERE account_id=:account"));
            clear.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
            if (!clear.exec())
                return queryError(QStringLiteral("Clear Sieve scripts"), clear);
            QSqlQuery insert{database};
            insert.prepare(QStringLiteral(
                "INSERT INTO sieve_scripts(account_id,script_id,name,blob_id,is_active) "
                "VALUES(:account,:id,:name,:blob,:active)"));
            for (const auto& script : scripts)
            {
                insert.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(std::string{accountId}));
                insert.bindValue(QStringLiteral(":id"), QString::fromStdString(script.id));
                insert.bindValue(QStringLiteral(":name"), QString::fromStdString(script.name));
                insert.bindValue(QStringLiteral(":blob"), QString::fromStdString(script.blobId));
                insert.bindValue(QStringLiteral(":active"), script.isActive ? 1 : 0);
                if (!insert.exec())
                    return queryError(QStringLiteral("Store Sieve script"), insert);
            }
            return std::nullopt;
        }
    } // namespace

    SieveRepository::SieveRepository(DatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    SieveRepository::replaceAll(const std::string_view accountId,
                                const std::vector<javelin::jmap::sieve::SieveScript>& scripts,
                                const std::string_view state)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Replace Sieve scripts"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = replaceAll(transaction, accountId, scripts, state))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    SieveRepository::replaceAll(DatabaseTransaction& transaction, const std::string_view accountId,
                                const std::vector<javelin::jmap::sieve::SieveScript>& scripts,
                                const std::string_view state)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Sieve replacement requires a matching transaction"),
            };
        if (const auto error = writeScripts(m_connection, accountId, scripts))
            return error;
        QSqlQuery syncState{m_connection.database()};
        syncState.prepare(
            QStringLiteral("INSERT INTO sync_state(account_id,object_type,query_key,state_token) "
                           "VALUES(:account,'SieveScript','',:state) "
                           "ON CONFLICT(account_id,object_type,query_key) DO UPDATE SET "
                           "state_token=excluded.state_token,updated_at=CURRENT_TIMESTAMP"));
        syncState.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
        syncState.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!syncState.exec())
            return queryError(QStringLiteral("Store Sieve state"), syncState);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    SieveRepository::project(DatabaseTransaction& transaction, const std::string_view accountId,
                             const std::vector<javelin::jmap::sieve::SieveScript>& scripts)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Sieve projection requires a matching transaction"),
            };
        return writeScripts(m_connection, accountId, scripts);
    }

    std::variant<std::vector<javelin::jmap::sieve::SieveScript>, DatabaseError>
    SieveRepository::list(const std::string_view accountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT script_id,name,blob_id,is_active FROM sieve_scripts "
                           "WHERE account_id=:account ORDER BY name COLLATE NOCASE,script_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("List Sieve scripts"), query);
        std::vector<javelin::jmap::sieve::SieveScript> scripts;
        while (query.next())
            scripts.push_back({
                .id = query.value(0).toString().toStdString(),
                .name = query.value(1).toString().toStdString(),
                .blobId = query.value(2).toString().toStdString(),
                .isActive = query.value(3).toInt() != 0,
            });
        return scripts;
    }

    std::variant<std::optional<std::string>, DatabaseError>
    SieveRepository::state(const std::string_view accountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT state_token FROM sync_state WHERE account_id=:account "
                                     "AND object_type='SieveScript' AND query_key=''"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read Sieve state"), query);
        if (!query.next())
            return std::optional<std::string>{};
        return std::optional<std::string>{query.value(0).toString().toStdString()};
    }
} // namespace javelin::jmap::cache
