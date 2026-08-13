#include "jmap/cache/SearchWindowReadRepository.h"

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

    SearchWindowReadRepository::SearchWindowReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    SearchWindowReadRepository::SearchWindowReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    SearchWindowReadRepository::SearchWindowReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    SearchWindowResult SearchWindowReadRepository::find(const std::string_view accountId,
                                                        const std::string_view queryKey,
                                                        const std::size_t offset,
                                                        const std::size_t limit) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery windowQuery{m_connection.database()};
        windowQuery.prepare(QStringLiteral(
            "SELECT position, returned_limit, total, query_state, coverage, materialization "
            "FROM search_windows WHERE account_id = :account_id AND query_key = :query_key "
            "AND window_offset = :offset AND window_limit = :limit"));
        bindKey(windowQuery, accountId, queryKey, offset, limit);
        if (!windowQuery.exec())
            return makeQueryError(QStringLiteral("Read search window"), windowQuery);
        if (!windowQuery.next())
            return std::optional<SearchWindowRecord>{std::nullopt};

        SearchWindowRecord record{
            .accountId = std::string{accountId},
            .queryKey = std::string{queryKey},
            .offset = offset,
            .limit = limit,
            .position = static_cast<std::size_t>(windowQuery.value(0).toULongLong()),
            .returnedLimit = static_cast<std::size_t>(windowQuery.value(1).toULongLong()),
            .total = windowQuery.value(2).isNull()
                         ? std::nullopt
                         : std::optional<std::size_t>{static_cast<std::size_t>(
                               windowQuery.value(2).toULongLong())},
            .queryState = windowQuery.value(3).toString().toStdString(),
            .coverage = coverageFromValue(windowQuery.value(4).toString()),
            .materialization = materializationFromValue(windowQuery.value(5).toString()),
            .emailIds = {},
        };

        QSqlQuery itemsQuery{m_connection.database()};
        itemsQuery.prepare(QStringLiteral(
            "SELECT email_id FROM search_window_items WHERE account_id = :account_id AND "
            "query_key = :query_key AND window_offset = :offset AND window_limit = :limit "
            "ORDER BY position"));
        bindKey(itemsQuery, accountId, queryKey, offset, limit);
        if (!itemsQuery.exec())
            return makeQueryError(QStringLiteral("Read search-window items"), itemsQuery);
        while (itemsQuery.next())
            record.emailIds.push_back(itemsQuery.value(0).toString().toStdString());
        return std::optional<SearchWindowRecord>{std::move(record)};
    }

} // namespace javelin::jmap::cache
