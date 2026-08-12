#include "jmap/cache/MailboxWindowReadRepository.h"

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

        [[nodiscard]] QueryWindowCoverage coverageFromValue(const QString& value)
        {
            if (value == QStringLiteral("locally_projected"))
                return QueryWindowCoverage::LocallyProjected;
            if (value == QStringLiteral("stale"))
                return QueryWindowCoverage::Stale;
            return QueryWindowCoverage::Server;
        }

        [[nodiscard]] QueryWindowMaterialization materializationFromValue(const QString& value)
        {
            return value == QStringLiteral("partial") ? QueryWindowMaterialization::Partial
                                                      : QueryWindowMaterialization::Complete;
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

    MailboxWindowReadRepository::MailboxWindowReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxWindowReadRepository::MailboxWindowReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxWindowReadRepository::MailboxWindowReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    MailboxWindowResult MailboxWindowReadRepository::find(const std::string_view accountId,
                                                          const std::string_view queryKey,
                                                          const std::size_t requestedOffset,
                                                          const std::size_t requestedLimit) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery windowQuery{m_connection.database()};
        windowQuery.prepare(QStringLiteral(
            "SELECT mailbox_id,position,returned_limit,total,query_state,coverage,materialization "
            "FROM mailbox_query_windows WHERE account_id=:account_id AND query_key=:query_key "
            "AND requested_offset=:requested_offset AND requested_limit=:requested_limit"));
        bindKey(windowQuery, accountId, queryKey, requestedOffset, requestedLimit);
        if (!windowQuery.exec())
            return makeQueryError(QStringLiteral("Read mailbox window"), windowQuery);
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
            .coverage = coverageFromValue(windowQuery.value(5).toString()),
            .materialization = materializationFromValue(windowQuery.value(6).toString()),
            .emailIds = {},
        };

        QSqlQuery itemsQuery{m_connection.database()};
        itemsQuery.prepare(QStringLiteral(
            "SELECT email_id FROM mailbox_query_window_items WHERE account_id=:account_id "
            "AND query_key=:query_key AND requested_offset=:requested_offset "
            "AND requested_limit=:requested_limit ORDER BY position"));
        bindKey(itemsQuery, accountId, queryKey, requestedOffset, requestedLimit);
        if (!itemsQuery.exec())
            return makeQueryError(QStringLiteral("Read mailbox-window items"), itemsQuery);
        while (itemsQuery.next())
            record.emailIds.push_back(itemsQuery.value(0).toString().toStdString());
        return std::optional<MailboxWindowRecord>{std::move(record)};
    }

} // namespace javelin::jmap::cache
