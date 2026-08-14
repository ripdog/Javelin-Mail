#include "jmap/cache/SyncStateRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        void bindKey(QSqlQuery& query, const SyncStateKey& key)
        {
            query.bindValue(QStringLiteral(":account_id"), QString::fromStdString(key.accountId));
            query.bindValue(QStringLiteral(":object_type"), QString::fromStdString(key.objectType));
            query.bindValue(QStringLiteral(":query_key"), QString::fromStdString(key.queryKey));
        }

    } // namespace

    SyncStateRepository::SyncStateRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> SyncStateRepository::upsert(const SyncStateKey& key,
                                                             std::string_view stateToken)
    {
        const DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO sync_state (account_id, object_type, query_key, state_token, updated_at) "
            "VALUES (:account_id, :object_type, :query_key, :state_token, CURRENT_TIMESTAMP) "
            "ON CONFLICT(account_id, object_type, query_key) DO UPDATE SET "
            "state_token = excluded.state_token, "
            "updated_at = CURRENT_TIMESTAMP"));
        bindKey(query, key);
        query.bindValue(QStringLiteral(":state_token"),
                        QString::fromStdString(std::string{stateToken}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Upsert sync_state"), query);
        }

        return std::nullopt;
    }

    std::optional<DatabaseError> SyncStateRepository::upsert(DatabaseTransaction& transaction,
                                                             const SyncStateKey& key,
                                                             const std::string_view stateToken)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Sync state upsert requires a matching transaction"),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO sync_state (account_id, object_type, query_key, state_token, updated_at) "
            "VALUES (:account_id, :object_type, :query_key, :state_token, CURRENT_TIMESTAMP) "
            "ON CONFLICT(account_id, object_type, query_key) DO UPDATE SET "
            "state_token = excluded.state_token, "
            "updated_at = CURRENT_TIMESTAMP"));
        bindKey(query, key);
        query.bindValue(QStringLiteral(":state_token"),
                        QString::fromStdString(std::string{stateToken}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Upsert sync_state"), query);
        return std::nullopt;
    }

    std::variant<bool, DatabaseError>
    SyncStateRepository::advanceIfCurrent(DatabaseTransaction& transaction, const SyncStateKey& key,
                                          const std::string_view expectedState,
                                          const std::string_view newState)
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Sync state advancement requires a matching transaction"),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE sync_state SET state_token=:new_state, updated_at=CURRENT_TIMESTAMP "
            "WHERE account_id=:account_id AND object_type=:object_type AND query_key=:query_key "
            "AND state_token=:expected_state"));
        bindKey(query, key);
        query.bindValue(QStringLiteral(":new_state"),
                        QString::fromStdString(std::string{newState}));
        query.bindValue(QStringLiteral(":expected_state"),
                        QString::fromStdString(std::string{expectedState}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Conditionally advance sync_state"), query);
        }
        return query.numRowsAffected() == 1;
    }

    std::variant<bool, DatabaseError>
    SyncStateRepository::replaceIfCurrent(DatabaseTransaction& transaction, const SyncStateKey& key,
                                          const std::optional<std::string_view> expectedState,
                                          const std::string_view newState)
    {
        if (expectedState.has_value())
            return advanceIfCurrent(transaction, key, *expectedState, newState);
        if (const auto error = m_connection.validate())
            return *error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Sync state replacement requires a matching transaction"),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO sync_state (account_id,object_type,query_key,state_token,updated_at) "
            "SELECT :account_id,:object_type,:query_key,:new_state,CURRENT_TIMESTAMP "
            "WHERE NOT EXISTS(SELECT 1 FROM sync_state WHERE account_id=:account_id AND "
            "object_type=:object_type AND query_key=:query_key)"));
        bindKey(query, key);
        query.bindValue(QStringLiteral(":new_state"),
                        QString::fromStdString(std::string{newState}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Conditionally initialize sync_state"), query);
        return query.numRowsAffected() == 1;
    }

    std::variant<std::optional<SyncStateRecord>, DatabaseError>
    SyncStateRepository::find(const SyncStateKey& key) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT state_token, updated_at "
            "FROM sync_state "
            "WHERE account_id = :account_id AND object_type = :object_type AND query_key = "
            ":query_key"));
        bindKey(query, key);
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read sync_state"), query);
        }

        if (!query.next())
        {
            return std::optional<SyncStateRecord>{std::nullopt};
        }

        return std::optional<SyncStateRecord>{SyncStateRecord{
            .key = key,
            .stateToken = query.value(0).toString().toStdString(),
            .updatedAt = query.value(1).toString().toStdString(),
        }};
    }

    std::optional<DatabaseError> SyncStateRepository::remove(const SyncStateKey& key)
    {
        const DatabaseWriteScope writeScope{m_connection};
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM sync_state "
            "WHERE account_id = :account_id AND object_type = :object_type AND query_key = "
            ":query_key"));
        bindKey(query, key);
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Delete sync_state"), query);
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::cache
