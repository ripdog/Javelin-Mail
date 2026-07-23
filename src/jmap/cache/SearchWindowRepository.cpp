#include "jmap/cache/SearchWindowRepository.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace javelin::jmap::cache
{

    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] DatabaseError databaseError(const QString& operation,
                                                  const QSqlDatabase& database)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + database.lastError().text(),
            };
        }

        void bindWindowKey(QSqlQuery& query, const std::string_view accountId,
                           const std::string_view queryKey, const std::size_t offset,
                           const std::size_t limit)
        {
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":query_key"),
                            QString::fromStdString(std::string{queryKey}));
            query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
            query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        }
    } // namespace

    SearchWindowRepository::SearchWindowRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> SearchWindowRepository::replace(const SearchWindowRecord& window)
    {
        if (const auto error = m_connection.validate())
        {
            return error;
        }

        const DatabaseWriteScope writeScope{m_connection};
        auto database = m_connection.database();
        if (!database.transaction())
        {
            return databaseError(QStringLiteral("Begin search-window transaction"), database);
        }

        QSqlQuery deleteStaleWindows{database};
        deleteStaleWindows.prepare(QStringLiteral(
            "DELETE FROM search_windows WHERE account_id=:account_id AND query_key=:query_key "
            "AND query_state<>:query_state"));
        deleteStaleWindows.bindValue(QStringLiteral(":account_id"),
                                     QString::fromStdString(window.accountId));
        deleteStaleWindows.bindValue(QStringLiteral(":query_key"),
                                     QString::fromStdString(window.queryKey));
        deleteStaleWindows.bindValue(QStringLiteral(":query_state"),
                                     QString::fromStdString(window.queryState));
        if (!deleteStaleWindows.exec())
        {
            const auto error =
                queryError(QStringLiteral("Invalidate stale search windows"), deleteStaleWindows);
            database.rollback();
            return error;
        }

        QSqlQuery replaceWindow{database};
        replaceWindow.prepare(QStringLiteral(
            "INSERT INTO search_windows "
            "(account_id, query_key, window_offset, window_limit, position, returned_limit, total, "
            "query_state, is_valid, updated_at) VALUES (:account_id, :query_key, :offset, :limit, "
            ":position, :returned_limit, :total, :query_state, 1, CURRENT_TIMESTAMP) "
            "ON CONFLICT(account_id, query_key, window_offset, window_limit) DO UPDATE SET "
            "position = excluded.position, returned_limit = excluded.returned_limit, "
            "total = excluded.total, query_state = excluded.query_state, is_valid = 1, "
            "updated_at = CURRENT_TIMESTAMP"));
        bindWindowKey(replaceWindow, window.accountId, window.queryKey, window.offset,
                      window.limit);
        replaceWindow.bindValue(QStringLiteral(":total"),
                                window.total.has_value()
                                    ? QVariant{static_cast<qulonglong>(*window.total)}
                                    : QVariant{});
        replaceWindow.bindValue(QStringLiteral(":position"),
                                static_cast<qulonglong>(window.position));
        replaceWindow.bindValue(QStringLiteral(":returned_limit"),
                                static_cast<qulonglong>(window.returnedLimit));
        replaceWindow.bindValue(QStringLiteral(":query_state"),
                                QString::fromStdString(window.queryState));
        if (!replaceWindow.exec())
        {
            const auto error = queryError(QStringLiteral("Replace search window"), replaceWindow);
            database.rollback();
            return error;
        }

        QSqlQuery deleteItems{database};
        deleteItems.prepare(QStringLiteral(
            "DELETE FROM search_window_items WHERE account_id = :account_id AND query_key = "
            ":query_key AND window_offset = :offset AND window_limit = :limit"));
        bindWindowKey(deleteItems, window.accountId, window.queryKey, window.offset, window.limit);
        if (!deleteItems.exec())
        {
            const auto error =
                queryError(QStringLiteral("Delete search-window items"), deleteItems);
            database.rollback();
            return error;
        }

        QSqlQuery insertItem{database};
        insertItem.prepare(QStringLiteral(
            "INSERT INTO search_window_items "
            "(account_id, query_key, window_offset, window_limit, position, email_id) "
            "VALUES (:account_id, :query_key, :offset, :limit, :position, :email_id)"));
        for (std::size_t position = 0; position < window.emailIds.size(); ++position)
        {
            bindWindowKey(insertItem, window.accountId, window.queryKey, window.offset,
                          window.limit);
            insertItem.bindValue(QStringLiteral(":position"), static_cast<qulonglong>(position));
            insertItem.bindValue(QStringLiteral(":email_id"),
                                 QString::fromStdString(window.emailIds[position]));
            if (!insertItem.exec())
            {
                const auto error =
                    queryError(QStringLiteral("Insert search-window item"), insertItem);
                database.rollback();
                return error;
            }
        }

        QSqlQuery evictWindows{database};
        evictWindows.prepare(QStringLiteral(
            "DELETE FROM search_windows WHERE (account_id,query_key,window_offset,window_limit) "
            "IN (SELECT account_id,query_key,window_offset,window_limit FROM search_windows WHERE "
            "account_id=:account_id AND query_key=:query_key ORDER BY updated_at DESC,"
            "window_offset DESC LIMIT -1 OFFSET 64)"));
        evictWindows.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(window.accountId));
        evictWindows.bindValue(QStringLiteral(":query_key"),
                               QString::fromStdString(window.queryKey));
        if (!evictWindows.exec())
        {
            const auto error = queryError(QStringLiteral("Evict old search windows"), evictWindows);
            database.rollback();
            return error;
        }

        if (!database.commit())
        {
            const auto error =
                databaseError(QStringLiteral("Commit search-window transaction"), database);
            database.rollback();
            return error;
        }

        return std::nullopt;
    }

    SearchWindowResult SearchWindowRepository::find(const std::string_view accountId,
                                                    const std::string_view queryKey,
                                                    const std::size_t offset,
                                                    const std::size_t limit) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery windowQuery{m_connection.database()};
        windowQuery.prepare(
            QStringLiteral("SELECT position, returned_limit, total, query_state, is_valid FROM "
                           "search_windows WHERE "
                           "account_id = :account_id AND query_key = "
                           ":query_key AND window_offset = :offset AND window_limit = :limit"));
        bindWindowKey(windowQuery, accountId, queryKey, offset, limit);
        if (!windowQuery.exec())
        {
            return queryError(QStringLiteral("Read search window"), windowQuery);
        }
        if (!windowQuery.next())
        {
            return std::optional<SearchWindowRecord>{std::nullopt};
        }

        SearchWindowRecord record{
            .accountId = std::string{accountId},
            .queryKey = std::string{queryKey},
            .offset = offset,
            .limit = limit,
            .position = static_cast<std::size_t>(windowQuery.value(0).toULongLong()),
            .returnedLimit = static_cast<std::size_t>(windowQuery.value(1).toULongLong()),
            .total = windowQuery.value(2).isNull()
                         ? std::optional<std::size_t>{std::nullopt}
                         : std::optional<std::size_t>{static_cast<std::size_t>(
                               windowQuery.value(2).toULongLong())},
            .queryState = windowQuery.value(3).toString().toStdString(),
            .isAuthoritative = windowQuery.value(4).toInt() != 0,
            .emailIds = {},
        };

        QSqlQuery itemsQuery{m_connection.database()};
        itemsQuery.prepare(QStringLiteral(
            "SELECT email_id FROM search_window_items WHERE account_id = :account_id AND "
            "query_key = :query_key AND window_offset = :offset AND window_limit = :limit "
            "ORDER BY position"));
        bindWindowKey(itemsQuery, accountId, queryKey, offset, limit);
        if (!itemsQuery.exec())
        {
            return queryError(QStringLiteral("Read search-window items"), itemsQuery);
        }
        while (itemsQuery.next())
        {
            record.emailIds.push_back(itemsQuery.value(0).toString().toStdString());
        }

        return std::optional<SearchWindowRecord>{std::move(record)};
    }

    std::optional<DatabaseError>
    SearchWindowRepository::invalidateAccount(DatabaseTransaction& transaction,
                                              const std::string_view accountId)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Search-window invalidation requires an active matching transaction"),
            };
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("UPDATE search_windows SET is_valid=0 WHERE account_id=:account_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Invalidate search windows"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    SearchWindowRepository::eraseQuery(const std::string_view accountId,
                                       const std::string_view queryKey)
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "DELETE FROM search_windows WHERE account_id=:account_id AND query_key=:query_key"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":query_key"),
                        QString::fromStdString(std::string{queryKey}));
        if (!query.exec())
            return queryError(QStringLiteral("Delete search session windows"), query);
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
