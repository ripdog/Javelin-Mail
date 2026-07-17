#include "jmap/cache/MailboxWindowRepository.h"

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

        QSqlQuery deleteStaleWindows{database};
        deleteStaleWindows.prepare(QStringLiteral(
            "DELETE FROM mailbox_query_windows WHERE account_id=:account_id AND "
            "mailbox_id=:mailbox_id AND query_key=:query_key AND query_state<>:query_state"));
        deleteStaleWindows.bindValue(QStringLiteral(":account_id"),
                                     QString::fromStdString(window.accountId));
        deleteStaleWindows.bindValue(QStringLiteral(":mailbox_id"),
                                     QString::fromStdString(window.mailboxId));
        deleteStaleWindows.bindValue(QStringLiteral(":query_key"),
                                     QString::fromStdString(window.queryKey));
        deleteStaleWindows.bindValue(QStringLiteral(":query_state"),
                                     QString::fromStdString(window.queryState));
        if (!deleteStaleWindows.exec())
        {
            const auto error =
                queryError(QStringLiteral("Invalidate stale mailbox windows"), deleteStaleWindows);
            database.rollback();
            return error;
        }

        QSqlQuery replaceWindow{database};
        replaceWindow.prepare(QStringLiteral(
            "INSERT INTO mailbox_query_windows "
            "(account_id,mailbox_id,query_key,requested_offset,requested_limit,position,"
            "returned_limit,total,query_state,updated_at) VALUES "
            "(:account_id,:mailbox_id,:query_key,:requested_offset,:requested_limit,:position,"
            ":returned_limit,:total,:query_state,CURRENT_TIMESTAMP) "
            "ON CONFLICT(account_id,query_key,requested_offset,requested_limit) DO UPDATE SET "
            "mailbox_id=excluded.mailbox_id,position=excluded.position,"
            "returned_limit=excluded.returned_limit,total=excluded.total,"
            "query_state=excluded.query_state,updated_at=CURRENT_TIMESTAMP"));
        bindKey(replaceWindow, window.accountId, window.queryKey, window.requestedOffset,
                window.requestedLimit);
        replaceWindow.bindValue(QStringLiteral(":mailbox_id"),
                                QString::fromStdString(window.mailboxId));
        replaceWindow.bindValue(QStringLiteral(":position"),
                                static_cast<qulonglong>(window.position));
        replaceWindow.bindValue(QStringLiteral(":returned_limit"),
                                static_cast<qulonglong>(window.returnedLimit));
        replaceWindow.bindValue(QStringLiteral(":total"),
                                window.total.has_value()
                                    ? QVariant{static_cast<qulonglong>(*window.total)}
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

        QSqlQuery evictWindows{database};
        evictWindows.prepare(QStringLiteral(
            "DELETE FROM mailbox_query_windows WHERE (account_id,query_key,requested_offset,"
            "requested_limit) IN (SELECT account_id,query_key,requested_offset,requested_limit "
            "FROM mailbox_query_windows WHERE account_id=:account_id AND query_key=:query_key "
            "ORDER BY updated_at DESC,requested_offset DESC LIMIT -1 OFFSET 12)"));
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
            "SELECT mailbox_id,position,returned_limit,total,query_state FROM "
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
        query.prepare(
            QStringLiteral("DELETE FROM mailbox_query_windows WHERE account_id=:account_id "
                           "AND mailbox_id=:mailbox_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return queryError(QStringLiteral("Invalidate mailbox windows"), query);
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
