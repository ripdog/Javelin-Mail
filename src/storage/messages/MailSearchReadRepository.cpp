#include "jmap/cache/MailSearchReadRepository.h"

#include "jmap/cache/EmailListSortSql.h"
#include "jmap/cache/MailSearchIndex.h"
#include "jmap/cache/MessageSummaryReadRepository.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_set>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }
    } // namespace

    MailSearchReadRepository::MailSearchReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailSearchReadRepository::MailSearchReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailSearchReadRepository::MailSearchReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    MailSearchReadRepository::searchCachedMessageText(const std::string_view accountId,
                                                      const std::string_view text,
                                                      const std::size_t limit,
                                                      const std::size_t offset) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (text.empty() || limit == 0)
            return std::vector<MessageListItem>{};

        MailSearchIndex index{m_connection};
        const auto indexedIds = index.search(accountId, text, (limit + offset) * 4);
        const auto* emailIds = std::get_if<std::vector<std::string>>(&indexedIds);
        if (emailIds == nullptr)
            return std::get<DatabaseError>(indexedIds);

        const auto itemResult =
            MessageSummaryReadRepository{m_connection}.listMessagesByEmailIds(accountId, *emailIds);
        if (const auto* error = std::get_if<DatabaseError>(&itemResult))
            return *error;

        std::vector<MessageListItem> results;
        std::unordered_set<std::string> threadIds;
        for (const auto& item : std::get<std::vector<MessageListItem>>(itemResult))
        {
            if (!threadIds.insert(item.threadId).second)
                continue;
            if (threadIds.size() > offset)
                results.push_back(item);
            if (results.size() == limit)
                break;
        }
        return results;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    MailSearchReadRepository::searchAllCachedMessageText(
        const std::string_view accountId, const std::string_view text,
        const javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (text.empty())
            return std::vector<MessageListItem>{};

        MailSearchIndex index{m_connection};
        const auto indexedIds = index.searchAll(accountId, text);
        const auto* emailIds = std::get_if<std::vector<std::string>>(&indexedIds);
        if (emailIds == nullptr)
            return std::get<DatabaseError>(indexedIds);
        return listSortedMessages(accountId, *emailIds, sort);
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    MailSearchReadRepository::listSortedMessages(
        const std::string_view accountId, const std::vector<std::string>& emailIds,
        const javelin::jmap::query::EmailListSort sort) const
    {
        if (emailIds.empty())
            return std::vector<MessageListItem>{};

        std::string emailIdsJson;
        if (const auto writeError = glz::write_json(emailIds, emailIdsJson))
        {
            Q_UNUSED(writeError);
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Serialize local-search ids failed."),
            };
        }

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = detail::emailListSortKeyExpression(sort.property);
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral(
                "WITH requested AS MATERIALIZED ("
                "  SELECT value AS email_id FROM json_each(:email_ids_json)"
                "), ranked_matches AS ("
                "  SELECT e.email_id, e.thread_id, %2 AS sort_key, "
                "         ROW_NUMBER() OVER (PARTITION BY e.thread_id "
                "                            ORDER BY %2 %1, e.email_id %1) AS thread_rank "
                "  FROM requested r "
                "  CROSS JOIN emails e ON e.account_id=:account_id AND e.email_id=r.email_id"
                ") "
                "SELECT email_id FROM ranked_matches WHERE thread_rank=1 "
                "ORDER BY sort_key %1, email_id %1")
                .arg(orderDirection, sortKey));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_ids_json"), QString::fromStdString(emailIdsJson));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Sort local-search results"), query);

        std::vector<std::string> sortedIds;
        sortedIds.reserve(emailIds.size());
        while (query.next())
            sortedIds.push_back(query.value(0).toString().toStdString());
        return MessageSummaryReadRepository{m_connection}.listMessagesByEmailIds(accountId,
                                                                                 sortedIds);
    }

} // namespace javelin::jmap::cache
