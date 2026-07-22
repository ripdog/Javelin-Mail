#include "jmap/cache/MailboxWindowRepository.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>
#include <unordered_set>

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

        void bindKey(QSqlQuery& query, const std::string_view accountId,
                     const std::string_view queryKey, const std::size_t requestedOffset,
                     const std::size_t requestedLimit)
        {
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":query_key"),
                            QString::fromStdString(std::string{queryKey}));
            query.bindValue(QStringLiteral(":requested_offset"),
                            static_cast<qulonglong>(requestedOffset));
            query.bindValue(QStringLiteral(":requested_limit"),
                            static_cast<qulonglong>(requestedLimit));
        }
    } // namespace

    MailboxWindowRepository::MailboxWindowRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> MailboxWindowRepository::replace(const MailboxWindowRecord& window)
    {
        if (const auto error = m_connection.validate())
            return error;

        auto database = m_connection.database();
        if (!database.transaction())
            return databaseError(QStringLiteral("Begin mailbox-window transaction"), database);

        auto effectiveTotal = window.total;
        if (!effectiveTotal.has_value())
        {
            QSqlQuery knownTotal{database};
            knownTotal.prepare(QStringLiteral(
                "SELECT total FROM mailbox_query_windows WHERE account_id=:account_id AND "
                "mailbox_id=:mailbox_id AND query_key=:query_key AND query_state=:query_state "
                "AND is_valid=1 AND total IS NOT NULL ORDER BY requested_offset LIMIT 1"));
            knownTotal.bindValue(QStringLiteral(":account_id"),
                                 QString::fromStdString(window.accountId));
            knownTotal.bindValue(QStringLiteral(":mailbox_id"),
                                 QString::fromStdString(window.mailboxId));
            knownTotal.bindValue(QStringLiteral(":query_key"),
                                 QString::fromStdString(window.queryKey));
            knownTotal.bindValue(QStringLiteral(":query_state"),
                                 QString::fromStdString(window.queryState));
            if (!knownTotal.exec())
            {
                const auto error =
                    queryError(QStringLiteral("Read known mailbox query total"), knownTotal);
                database.rollback();
                return error;
            }
            if (knownTotal.next())
                effectiveTotal = static_cast<std::size_t>(knownTotal.value(0).toULongLong());
        }

        QSqlQuery offlineScope{database};
        offlineScope.prepare(
            QStringLiteral("SELECT 1 FROM offline_mailbox_scopes WHERE account_id=:account_id AND "
                           "mailbox_id=:mailbox_id AND desired=1"));
        offlineScope.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(window.accountId));
        offlineScope.bindValue(QStringLiteral(":mailbox_id"),
                               QString::fromStdString(window.mailboxId));
        if (!offlineScope.exec())
        {
            const auto error =
                queryError(QStringLiteral("Inspect offline mailbox scope"), offlineScope);
            database.rollback();
            return error;
        }
        const bool retainAllWindows = offlineScope.next();
        offlineScope.finish();

        QSqlQuery staleWindows{database};
        staleWindows.prepare(
            QStringLiteral(
                "%1 mailbox_query_windows %2 WHERE account_id=:account_id AND "
                "mailbox_id=:mailbox_id AND query_key=:query_key AND query_state<>:query_state")
                .arg(retainAllWindows ? QStringLiteral("UPDATE") : QStringLiteral("DELETE FROM"),
                     retainAllWindows ? QStringLiteral("SET is_valid=0") : QString{}));
        staleWindows.bindValue(QStringLiteral(":account_id"),
                               QString::fromStdString(window.accountId));
        staleWindows.bindValue(QStringLiteral(":mailbox_id"),
                               QString::fromStdString(window.mailboxId));
        staleWindows.bindValue(QStringLiteral(":query_key"),
                               QString::fromStdString(window.queryKey));
        staleWindows.bindValue(QStringLiteral(":query_state"),
                               QString::fromStdString(window.queryState));
        if (!staleWindows.exec())
        {
            const auto error =
                queryError(QStringLiteral("Invalidate stale mailbox windows"), staleWindows);
            database.rollback();
            return error;
        }

        QSqlQuery replaceWindow{database};
        replaceWindow.prepare(QStringLiteral(
            "INSERT INTO mailbox_query_windows "
            "(account_id,mailbox_id,query_key,requested_offset,requested_limit,position,"
            "returned_limit,total,query_state,is_valid,updated_at) VALUES "
            "(:account_id,:mailbox_id,:query_key,:requested_offset,:requested_limit,:position,"
            ":returned_limit,:total,:query_state,1,CURRENT_TIMESTAMP) "
            "ON CONFLICT(account_id,query_key,requested_offset,requested_limit) DO UPDATE SET "
            "mailbox_id=excluded.mailbox_id,position=excluded.position,"
            "returned_limit=excluded.returned_limit,total=excluded.total,"
            "query_state=excluded.query_state,is_valid=1,updated_at=CURRENT_TIMESTAMP"));
        bindKey(replaceWindow, window.accountId, window.queryKey, window.requestedOffset,
                window.requestedLimit);
        replaceWindow.bindValue(QStringLiteral(":mailbox_id"),
                                QString::fromStdString(window.mailboxId));
        replaceWindow.bindValue(QStringLiteral(":position"),
                                static_cast<qulonglong>(window.position));
        replaceWindow.bindValue(QStringLiteral(":returned_limit"),
                                static_cast<qulonglong>(window.returnedLimit));
        replaceWindow.bindValue(QStringLiteral(":total"),
                                effectiveTotal.has_value()
                                    ? QVariant{static_cast<qulonglong>(*effectiveTotal)}
                                    : QVariant{});
        replaceWindow.bindValue(QStringLiteral(":query_state"),
                                QString::fromStdString(window.queryState));
        if (!replaceWindow.exec())
        {
            const auto error = queryError(QStringLiteral("Replace mailbox window"), replaceWindow);
            database.rollback();
            return error;
        }

        QSqlQuery deleteItems{database};
        deleteItems.prepare(QStringLiteral(
            "DELETE FROM mailbox_query_window_items WHERE account_id=:account_id AND "
            "query_key=:query_key AND requested_offset=:requested_offset AND "
            "requested_limit=:requested_limit"));
        bindKey(deleteItems, window.accountId, window.queryKey, window.requestedOffset,
                window.requestedLimit);
        if (!deleteItems.exec())
        {
            const auto error =
                queryError(QStringLiteral("Delete mailbox-window items"), deleteItems);
            database.rollback();
            return error;
        }

        QSqlQuery insertItem{database};
        insertItem.prepare(QStringLiteral(
            "INSERT INTO mailbox_query_window_items "
            "(account_id,query_key,requested_offset,requested_limit,position,email_id) VALUES "
            "(:account_id,:query_key,:requested_offset,:requested_limit,:position,:email_id)"));
        for (std::size_t position = 0; position < window.emailIds.size(); ++position)
        {
            bindKey(insertItem, window.accountId, window.queryKey, window.requestedOffset,
                    window.requestedLimit);
            insertItem.bindValue(QStringLiteral(":position"), static_cast<qulonglong>(position));
            insertItem.bindValue(QStringLiteral(":email_id"),
                                 QString::fromStdString(window.emailIds[position]));
            if (!insertItem.exec())
            {
                const auto error =
                    queryError(QStringLiteral("Insert mailbox-window item"), insertItem);
                database.rollback();
                return error;
            }
        }

        if (!retainAllWindows)
        {
            QSqlQuery evictWindows{database};
            evictWindows.prepare(QStringLiteral(
                "DELETE FROM mailbox_query_windows WHERE (account_id,query_key,requested_offset,"
                "requested_limit) IN (SELECT account_id,query_key,requested_offset,"
                "requested_limit FROM mailbox_query_windows WHERE account_id=:account_id AND "
                "query_key=:query_key ORDER BY updated_at DESC,requested_offset DESC LIMIT -1 "
                "OFFSET 12)"));
            evictWindows.bindValue(QStringLiteral(":account_id"),
                                   QString::fromStdString(window.accountId));
            evictWindows.bindValue(QStringLiteral(":query_key"),
                                   QString::fromStdString(window.queryKey));
            if (!evictWindows.exec())
            {
                const auto error =
                    queryError(QStringLiteral("Evict old mailbox windows"), evictWindows);
                database.rollback();
                return error;
            }
        }

        if (!database.commit())
        {
            const auto error =
                databaseError(QStringLiteral("Commit mailbox-window transaction"), database);
            database.rollback();
            return error;
        }
        return std::nullopt;
    }

    MailboxWindowResult MailboxWindowRepository::find(const std::string_view accountId,
                                                      const std::string_view queryKey,
                                                      const std::size_t requestedOffset,
                                                      const std::size_t requestedLimit) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery windowQuery{m_connection.database()};
        windowQuery.prepare(QStringLiteral(
            "SELECT mailbox_id,position,returned_limit,total,query_state,is_valid FROM "
            "mailbox_query_windows WHERE account_id=:account_id AND query_key=:query_key AND "
            "requested_offset=:requested_offset AND requested_limit=:requested_limit"));
        bindKey(windowQuery, accountId, queryKey, requestedOffset, requestedLimit);
        if (!windowQuery.exec())
            return queryError(QStringLiteral("Read mailbox window"), windowQuery);
        if (!windowQuery.next())
            return std::optional<MailboxWindowRecord>{std::nullopt};

        MailboxWindowRecord record{
            .accountId = std::string{accountId},
            .mailboxId = windowQuery.value(0).toString().toStdString(),
            .queryKey = std::string{queryKey},
            .requestedOffset = requestedOffset,
            .requestedLimit = requestedLimit,
            .position = static_cast<std::size_t>(windowQuery.value(1).toULongLong()),
            .returnedLimit = static_cast<std::size_t>(windowQuery.value(2).toULongLong()),
            .total = windowQuery.value(3).isNull()
                         ? std::nullopt
                         : std::optional<std::size_t>{static_cast<std::size_t>(
                               windowQuery.value(3).toULongLong())},
            .queryState = windowQuery.value(4).toString().toStdString(),
            .isAuthoritative = windowQuery.value(5).toInt() != 0,
            .emailIds = {},
        };

        QSqlQuery itemsQuery{m_connection.database()};
        itemsQuery.prepare(QStringLiteral(
            "SELECT email_id FROM mailbox_query_window_items WHERE account_id=:account_id AND "
            "query_key=:query_key AND requested_offset=:requested_offset AND "
            "requested_limit=:requested_limit ORDER BY position"));
        bindKey(itemsQuery, accountId, queryKey, requestedOffset, requestedLimit);
        if (!itemsQuery.exec())
            return queryError(QStringLiteral("Read mailbox-window items"), itemsQuery);
        while (itemsQuery.next())
            record.emailIds.push_back(itemsQuery.value(0).toString().toStdString());
        return std::optional<MailboxWindowRecord>{std::move(record)};
    }

    std::optional<DatabaseError>
    MailboxWindowRepository::invalidateMailbox(const std::string_view accountId,
                                               const std::string_view mailboxId)
    {
        if (const auto error = m_connection.validate())
            return error;
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Invalidate mailbox windows"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        if (const auto error = invalidateMailbox(transaction, accountId, mailboxId))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    MailboxWindowRepository::invalidateMailbox(DatabaseTransaction& transaction,
                                               const std::string_view accountId,
                                               const std::string_view mailboxId)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Mailbox-window invalidation requires an active matching transaction"),
            };
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE mailbox_query_windows SET is_valid=0 WHERE account_id=:account_id "
            "AND mailbox_id=:mailbox_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return queryError(QStringLiteral("Invalidate mailbox windows"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError> MailboxWindowRepository::rebaseContiguousPrefix(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view mailboxId, const std::string_view queryKey,
        const std::string_view sinceQueryState, const std::string_view newQueryState,
        std::vector<MailboxWindowAddition> additions, std::vector<std::string> removals,
        const std::optional<std::size_t> total)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Mailbox-window rebasing requires an active matching transaction"),
            };
        }

        struct Window
        {
            std::size_t offset = 0;
            std::size_t limit = 0;
        };
        QSqlQuery readWindows{m_connection.database()};
        readWindows.prepare(QStringLiteral(
            "SELECT requested_offset,requested_limit FROM mailbox_query_windows WHERE "
            "account_id=:account AND mailbox_id=:mailbox AND query_key=:query_key AND "
            "query_state=:query_state AND is_valid=1 ORDER BY requested_offset"));
        readWindows.bindValue(QStringLiteral(":account"),
                              QString::fromStdString(std::string{accountId}));
        readWindows.bindValue(QStringLiteral(":mailbox"),
                              QString::fromStdString(std::string{mailboxId}));
        readWindows.bindValue(QStringLiteral(":query_key"),
                              QString::fromStdString(std::string{queryKey}));
        readWindows.bindValue(QStringLiteral(":query_state"),
                              QString::fromStdString(std::string{sinceQueryState}));
        if (!readWindows.exec())
            return queryError(QStringLiteral("Read mailbox prefix windows"), readWindows);
        std::vector<Window> windows;
        std::size_t coveredEnd = 0;
        while (readWindows.next())
        {
            const Window candidate{
                .offset = readWindows.value(0).toULongLong(),
                .limit = readWindows.value(1).toULongLong(),
            };
            if (candidate.offset != coveredEnd || candidate.limit == 0)
                break;
            windows.push_back(candidate);
            coveredEnd += candidate.limit;
        }
        readWindows.finish();
        if (windows.empty())
            return std::nullopt;

        QSqlQuery readItems{m_connection.database()};
        readItems.prepare(QStringLiteral(
            "SELECT i.email_id FROM mailbox_query_window_items i INNER JOIN "
            "mailbox_query_windows w ON w.account_id=i.account_id AND w.query_key=i.query_key "
            "AND w.requested_offset=i.requested_offset AND "
            "w.requested_limit=i.requested_limit WHERE w.account_id=:account AND "
            "w.mailbox_id=:mailbox AND w.query_key=:query_key AND w.query_state=:query_state "
            "AND w.is_valid=1 AND w.requested_offset<:covered_end ORDER BY "
            "w.requested_offset,i.position"));
        readItems.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
        readItems.bindValue(QStringLiteral(":mailbox"),
                            QString::fromStdString(std::string{mailboxId}));
        readItems.bindValue(QStringLiteral(":query_key"),
                            QString::fromStdString(std::string{queryKey}));
        readItems.bindValue(QStringLiteral(":query_state"),
                            QString::fromStdString(std::string{sinceQueryState}));
        readItems.bindValue(QStringLiteral(":covered_end"), static_cast<qulonglong>(coveredEnd));
        if (!readItems.exec())
            return queryError(QStringLiteral("Read mailbox prefix items"), readItems);
        std::vector<std::string> ids;
        ids.reserve(coveredEnd);
        while (readItems.next())
            ids.push_back(readItems.value(0).toString().toStdString());

        const std::unordered_set<std::string> removed(removals.begin(), removals.end());
        std::erase_if(ids, [&removed](const auto& id) { return removed.contains(id); });
        std::ranges::sort(additions, {}, &MailboxWindowAddition::index);
        for (auto& addition : additions)
        {
            std::erase(ids, addition.emailId);
            if (addition.index <= ids.size() && addition.index < coveredEnd)
                ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(addition.index),
                           std::move(addition.emailId));
        }
        if (ids.size() > coveredEnd)
            ids.resize(coveredEnd);
        if (total.has_value() && ids.size() > *total)
            ids.resize(*total);

        QSqlQuery updateWindow{m_connection.database()};
        updateWindow.prepare(QStringLiteral(
            "UPDATE mailbox_query_windows SET query_state=:new_state,total=:total,"
            "returned_limit=:returned_limit,is_valid=:is_valid,updated_at=CURRENT_TIMESTAMP "
            "WHERE account_id=:account AND query_key=:query_key AND requested_offset=:offset AND "
            "requested_limit=:limit"));
        QSqlQuery deleteItems{m_connection.database()};
        deleteItems.prepare(QStringLiteral(
            "DELETE FROM mailbox_query_window_items WHERE account_id=:account AND "
            "query_key=:query_key AND requested_offset=:offset AND requested_limit=:limit"));
        QSqlQuery insertItem{m_connection.database()};
        insertItem.prepare(QStringLiteral(
            "INSERT INTO mailbox_query_window_items(account_id,query_key,requested_offset,"
            "requested_limit,position,email_id) VALUES(:account,:query_key,:offset,:limit,"
            ":position,:email_id)"));
        for (const auto& window : windows)
        {
            const auto bindWindow = [&](QSqlQuery& query)
            {
                query.bindValue(QStringLiteral(":account"),
                                QString::fromStdString(std::string{accountId}));
                query.bindValue(QStringLiteral(":query_key"),
                                QString::fromStdString(std::string{queryKey}));
                query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(window.offset));
                query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(window.limit));
            };
            bindWindow(updateWindow);
            updateWindow.bindValue(QStringLiteral(":new_state"),
                                   QString::fromStdString(std::string{newQueryState}));
            updateWindow.bindValue(QStringLiteral(":total"),
                                   total.has_value() ? QVariant{static_cast<qulonglong>(*total)}
                                                     : QVariant{});
            const auto end = std::min(ids.size(), window.offset + window.limit);
            const auto returnedLimit = end > window.offset ? end - window.offset : 0;
            const bool isAuthoritative =
                returnedLimit == window.limit ||
                (total.has_value() && window.offset + returnedLimit >= *total);
            updateWindow.bindValue(QStringLiteral(":returned_limit"),
                                   static_cast<qulonglong>(returnedLimit));
            updateWindow.bindValue(QStringLiteral(":is_valid"), isAuthoritative ? 1 : 0);
            if (!updateWindow.exec())
                return queryError(QStringLiteral("Rebase mailbox prefix window"), updateWindow);
            bindWindow(deleteItems);
            if (!deleteItems.exec())
                return queryError(QStringLiteral("Clear rebased mailbox prefix items"),
                                  deleteItems);
            for (std::size_t index = window.offset; index < end; ++index)
            {
                bindWindow(insertItem);
                insertItem.bindValue(QStringLiteral(":position"),
                                     static_cast<qulonglong>(index - window.offset));
                insertItem.bindValue(QStringLiteral(":email_id"),
                                     QString::fromStdString(ids[index]));
                if (!insertItem.exec())
                    return queryError(QStringLiteral("Write rebased mailbox prefix item"),
                                      insertItem);
            }
        }

        QSqlQuery invalidateSparse{m_connection.database()};
        invalidateSparse.prepare(QStringLiteral(
            "UPDATE mailbox_query_windows SET is_valid=0 WHERE account_id=:account AND "
            "mailbox_id=:mailbox AND query_key=:query_key AND query_state=:old_state AND "
            "requested_offset>=:covered_end"));
        invalidateSparse.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(std::string{accountId}));
        invalidateSparse.bindValue(QStringLiteral(":mailbox"),
                                   QString::fromStdString(std::string{mailboxId}));
        invalidateSparse.bindValue(QStringLiteral(":query_key"),
                                   QString::fromStdString(std::string{queryKey}));
        invalidateSparse.bindValue(QStringLiteral(":old_state"),
                                   QString::fromStdString(std::string{sinceQueryState}));
        invalidateSparse.bindValue(QStringLiteral(":covered_end"),
                                   static_cast<qulonglong>(coveredEnd));
        if (!invalidateSparse.exec())
            return queryError(QStringLiteral("Invalidate sparse mailbox windows"),
                              invalidateSparse);
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
