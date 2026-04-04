#include "jmap/cache/SyncStateRepository.h"

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

        void bindKey(QSqlQuery& query, const SyncStateKey& key)
        {
            query.bindValue(":account_id", QString::fromStdString(key.accountId));
            query.bindValue(":object_type", QString::fromStdString(key.objectType));
            query.bindValue(":query_key", QString::fromStdString(key.queryKey));
        }

    } // namespace

    SyncStateRepository::SyncStateRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> SyncStateRepository::upsert(const SyncStateKey& key,
                                                             std::string_view stateToken)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "INSERT INTO sync_state (account_id, object_type, query_key, state_token, updated_at) "
            "VALUES (:account_id, :object_type, :query_key, :state_token, CURRENT_TIMESTAMP) "
            "ON CONFLICT(account_id, object_type, query_key) DO UPDATE SET "
            "state_token = excluded.state_token, "
            "updated_at = CURRENT_TIMESTAMP");
        bindKey(query, key);
        query.bindValue(":state_token", QString::fromStdString(std::string{stateToken}));
        if (!query.exec())
        {
            return makeQueryError("Upsert sync_state", query);
        }

        return std::nullopt;
    }

    std::variant<std::optional<SyncStateRecord>, DatabaseError>
    SyncStateRepository::find(const SyncStateKey& key) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "SELECT state_token, updated_at "
            "FROM sync_state "
            "WHERE account_id = :account_id AND object_type = :object_type AND query_key = "
            ":query_key");
        bindKey(query, key);
        if (!query.exec())
        {
            return makeQueryError("Read sync_state", query);
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
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "DELETE FROM sync_state "
            "WHERE account_id = :account_id AND object_type = :object_type AND query_key = "
            ":query_key");
        bindKey(query, key);
        if (!query.exec())
        {
            return makeQueryError("Delete sync_state", query);
        }

        return std::nullopt;
    }

} // namespace javelin::jmap::cache
