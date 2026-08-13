#include "jmap/cache/MailboxFilterReadRepository.h"

#include "jmap/cache/EmailListSortSql.h"
#include "jmap/cache/MailSearchIndex.h"
#include "jmap/cache/MailTagReadRepository.h"
#include "jmap/cache/MessageSummaryReadRepository.h"

#include <glaze/glaze.hpp>

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        struct FilteredMailboxPredicate
        {
            QString sql;
            QString quickPattern;
            QString tagsJson;
            QString bodyIdsJson;
            std::size_t selectedTagCount = 0;
        };

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        [[nodiscard]] std::variant<FilteredMailboxPredicate, DatabaseError>
        filteredMailboxPredicate(const DatabaseReadView& connection,
                                 const std::string_view accountId,
                                 const javelin::jmap::search::EmailSearchCriteria& criteria)
        {
            FilteredMailboxPredicate result;
            if (criteria.unreadOnly)
            {
                result.sql += QStringLiteral(" AND NOT EXISTS(SELECT 1 FROM email_keywords k WHERE "
                                             "k.account_id=e.account_id "
                                             "AND k.email_id=e.email_id AND k.keyword='$seen')");
            }
            if (criteria.starredOnly)
            {
                result.sql += QStringLiteral(
                    " AND EXISTS(SELECT 1 FROM email_keywords k WHERE k.account_id=e.account_id "
                    "AND k.email_id=e.email_id AND k.keyword='$flagged')");
            }
            if (criteria.hasAttachmentOnly)
                result.sql += QStringLiteral(" AND e.has_attachment=1");
            if (criteria.fromContactsOnly)
            {
                result.sql += QStringLiteral(
                    " AND EXISTS(SELECT 1 FROM email_addresses a JOIN contact_emails c "
                    "ON LOWER(c.address)=LOWER(a.address) "
                    "WHERE a.account_id=e.account_id AND a.email_id=e.email_id "
                    "AND a.field_name='from')");
            }

            if (!criteria.tags.empty())
            {
                std::string tagsJson;
                if (const auto error = glz::write_json(criteria.tags, tagsJson))
                {
                    Q_UNUSED(error);
                    return DatabaseError{
                        .code = DatabaseErrorCode::QueryFailed,
                        .message = QStringLiteral("Serialize quick-filter tags failed."),
                    };
                }
                result.tagsJson = QString::fromStdString(tagsJson);
                result.selectedTagCount = criteria.tags.size();
                if (criteria.matchAllTags)
                {
                    result.sql += QStringLiteral(
                        " AND (SELECT COUNT(DISTINCT k.keyword) FROM email_keywords k "
                        "JOIN json_each(:filter_tags) t ON t.value=k.keyword "
                        "WHERE k.account_id=e.account_id AND k.email_id=e.email_id)="
                        ":filter_tag_count");
                }
                else
                {
                    result.sql += QStringLiteral(
                        " AND EXISTS(SELECT 1 FROM email_keywords k "
                        "JOIN json_each(:filter_tags) t ON t.value=k.keyword "
                        "WHERE k.account_id=e.account_id AND k.email_id=e.email_id)");
                }
            }
            else if (criteria.taggedOnly)
            {
                const auto keywordsResult =
                    MailTagReadRepository{connection}.listTagKeywords(accountId);
                if (const auto* error = std::get_if<DatabaseError>(&keywordsResult))
                    return *error;
                const auto& keywords = std::get<std::vector<std::string>>(keywordsResult);
                if (keywords.empty())
                {
                    result.sql += QStringLiteral(" AND 0");
                }
                else
                {
                    std::string tagsJson;
                    if (const auto error = glz::write_json(keywords, tagsJson))
                    {
                        Q_UNUSED(error);
                        return DatabaseError{
                            .code = DatabaseErrorCode::QueryFailed,
                            .message = QStringLiteral("Serialize quick-filter tags failed."),
                        };
                    }
                    result.tagsJson = QString::fromStdString(tagsJson);
                    result.sql += QStringLiteral(
                        " AND EXISTS(SELECT 1 FROM email_keywords k "
                        "JOIN json_each(:filter_tags) t ON t.value=k.keyword "
                        "WHERE k.account_id=e.account_id AND k.email_id=e.email_id)");
                }
            }

            const auto quickText = criteria.quickText.has_value()
                                       ? QString::fromStdString(*criteria.quickText).trimmed()
                                       : QString{};
            if (!quickText.isEmpty())
            {
                result.quickPattern = QStringLiteral("%") + quickText + QStringLiteral("%");
                QStringList scopes;
                if (criteria.quickTextSender)
                {
                    scopes.push_back(QStringLiteral(
                        "EXISTS(SELECT 1 FROM email_addresses a WHERE a.account_id=e.account_id "
                        "AND a.email_id=e.email_id AND a.field_name='from' AND "
                        "(a.address LIKE :quick_pattern OR COALESCE(a.display_name,'') LIKE "
                        ":quick_pattern))"));
                }
                if (criteria.quickTextRecipients)
                {
                    scopes.push_back(QStringLiteral(
                        "EXISTS(SELECT 1 FROM email_addresses a WHERE a.account_id=e.account_id "
                        "AND a.email_id=e.email_id AND a.field_name IN ('to','cc','bcc') AND "
                        "(a.address LIKE :quick_pattern OR COALESCE(a.display_name,'') LIKE "
                        ":quick_pattern))"));
                }
                if (criteria.quickTextSubject)
                    scopes.push_back(QStringLiteral("COALESCE(e.subject,'') LIKE :quick_pattern"));
                if (criteria.quickTextBody)
                {
                    MailSearchIndex index{connection};
                    const auto bodyResult = index.searchAllBody(accountId, quickText.toStdString());
                    const auto* bodyIds = std::get_if<std::vector<std::string>>(&bodyResult);
                    if (bodyIds == nullptr)
                        return std::get<DatabaseError>(bodyResult);
                    std::string bodyIdsJson;
                    if (const auto error = glz::write_json(*bodyIds, bodyIdsJson))
                    {
                        Q_UNUSED(error);
                        return DatabaseError{
                            .code = DatabaseErrorCode::QueryFailed,
                            .message =
                                QStringLiteral("Serialize quick-filter body matches failed."),
                        };
                    }
                    result.bodyIdsJson = QString::fromStdString(bodyIdsJson);
                    scopes.push_back(QStringLiteral(
                        "e.email_id IN (SELECT value FROM json_each(:filter_body_ids))"));
                }
                if (scopes.isEmpty())
                    scopes.push_back(QStringLiteral("COALESCE(e.subject,'') LIKE :quick_pattern"));
                result.sql += QStringLiteral(" AND (") + scopes.join(QStringLiteral(" OR ")) +
                              QStringLiteral(")");
            }
            return result;
        }

        void bindFilteredMailboxPredicate(QSqlQuery& query,
                                          const FilteredMailboxPredicate& predicate)
        {
            if (!predicate.quickPattern.isEmpty())
                query.bindValue(QStringLiteral(":quick_pattern"), predicate.quickPattern);
            if (!predicate.tagsJson.isEmpty())
            {
                query.bindValue(QStringLiteral(":filter_tags"), predicate.tagsJson);
                query.bindValue(QStringLiteral(":filter_tag_count"),
                                static_cast<qulonglong>(predicate.selectedTagCount));
            }
            if (!predicate.bodyIdsJson.isEmpty())
                query.bindValue(QStringLiteral(":filter_body_ids"), predicate.bodyIdsJson);
        }
    } // namespace

    MailboxFilterReadRepository::MailboxFilterReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxFilterReadRepository::MailboxFilterReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxFilterReadRepository::MailboxFilterReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    MailboxFilterReadRepository::listFilteredMailboxMessages(
        const std::string_view accountId, const std::string_view mailboxId,
        const javelin::jmap::search::EmailSearchCriteria& criteria, const std::size_t limit,
        const std::size_t offset, const javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (limit == 0)
            return std::vector<MessageListItem>{};

        const auto predicateResult = filteredMailboxPredicate(m_connection, accountId, criteria);
        const auto* predicate = std::get_if<FilteredMailboxPredicate>(&predicateResult);
        if (predicate == nullptr)
            return std::get<DatabaseError>(predicateResult);

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = detail::emailListSortKeyExpression(sort.property);
        QString sql = QStringLiteral(
            "WITH matching AS MATERIALIZED ("
            " SELECT e.email_id,e.thread_id,%2 AS sort_key FROM emails e "
            " JOIN email_mailboxes em ON em.account_id=e.account_id AND em.email_id=e.email_id "
            " WHERE e.account_id=:account_id AND em.mailbox_id=:mailbox_id");
        sql += predicate->sql;
        sql += QStringLiteral(
            "), ranked AS ("
            " SELECT email_id,thread_id,sort_key,ROW_NUMBER() OVER(PARTITION BY thread_id "
            " ORDER BY sort_key %1,email_id %1) AS thread_rank FROM matching"
            ") SELECT email_id FROM ranked WHERE thread_rank=1 "
            "ORDER BY sort_key %1,email_id %1 LIMIT :limit OFFSET :offset");
        QSqlQuery query{m_connection.database()};
        query.prepare(sql.arg(orderDirection, sortKey));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
        bindFilteredMailboxPredicate(query, *predicate);
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read quick-filtered mailbox messages"), query);

        std::vector<std::string> emailIds;
        emailIds.reserve(limit);
        while (query.next())
            emailIds.push_back(query.value(0).toString().toStdString());
        return MessageSummaryReadRepository{m_connection}.listMessagesByEmailIds(accountId,
                                                                                 emailIds);
    }

    std::variant<std::size_t, DatabaseError>
    MailboxFilterReadRepository::countFilteredMailboxMessages(
        const std::string_view accountId, const std::string_view mailboxId,
        const javelin::jmap::search::EmailSearchCriteria& criteria) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        const auto predicateResult = filteredMailboxPredicate(m_connection, accountId, criteria);
        const auto* predicate = std::get_if<FilteredMailboxPredicate>(&predicateResult);
        if (predicate == nullptr)
            return std::get<DatabaseError>(predicateResult);

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT COUNT(DISTINCT e.thread_id) FROM emails e "
                                     "JOIN email_mailboxes em ON em.account_id=e.account_id "
                                     "AND em.email_id=e.email_id WHERE e.account_id=:account_id "
                                     "AND em.mailbox_id=:mailbox_id") +
                      predicate->sql);
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        bindFilteredMailboxPredicate(query, *predicate);
        if (!query.exec())
            return makeQueryError(QStringLiteral("Count quick-filtered mailbox messages"), query);
        if (!query.next())
            return static_cast<std::size_t>(0);
        return static_cast<std::size_t>(query.value(0).toULongLong());
    }

} // namespace javelin::jmap::cache
