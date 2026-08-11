#include "jmap/cache/QueryService.h"

#include "jmap/cache/MailSearchIndex.h"
#include "jmap/cache/MailboxWindowRepository.h"

#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/domain/MailKeywords.h"

#include <glaze/glaze.hpp>

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QSqlError>
#include <QSqlQuery>

#include <unordered_map>
#include <unordered_set>

namespace javelin::jmap::cache
{
    Q_LOGGING_CATEGORY(logQueryPerformance, "jmap.cache.query")

    namespace
    {

        [[nodiscard]] DatabaseError makeQueryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }

        [[nodiscard]] std::optional<std::uint64_t> optionalCount(const QVariant& value)
        {
            return value.isNull() ? std::nullopt
                                  : std::optional<std::uint64_t>{value.toULongLong()};
        }

        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        queryUserKeywords(const DatabaseReadView& connection, const std::string_view accountId,
                          const std::string_view mailboxId = {})
        {
            QSqlQuery query{connection.database()};
            if (mailboxId.empty())
            {
                query.prepare(QStringLiteral(
                    "SELECT DISTINCT k.keyword FROM email_keywords k "
                    "WHERE k.account_id=:account_id AND NOT EXISTS(SELECT 1 FROM background_jobs j "
                    "WHERE j.account_id=k.account_id AND j.kind='tag_deletion' "
                    "AND j.status NOT IN ('failed','complete') "
                    "AND json_extract(j.checkpoint_json,'$.keyword')=k.keyword COLLATE NOCASE) "
                    "ORDER BY k.keyword COLLATE NOCASE"));
            }
            else
            {
                query.prepare(QStringLiteral(
                    "SELECT DISTINCT k.keyword FROM email_keywords k "
                    "JOIN email_mailboxes em ON em.account_id=k.account_id AND "
                    "em.email_id=k.email_id "
                    "WHERE k.account_id=:account_id AND em.mailbox_id=:mailbox_id "
                    "AND NOT EXISTS(SELECT 1 FROM background_jobs j "
                    "WHERE j.account_id=k.account_id AND j.kind='tag_deletion' "
                    "AND j.status NOT IN ('failed','complete') "
                    "AND json_extract(j.checkpoint_json,'$.keyword')=k.keyword COLLATE NOCASE) "
                    "ORDER BY k.keyword COLLATE NOCASE"));
                query.bindValue(QStringLiteral(":mailbox_id"),
                                QString::fromStdString(std::string{mailboxId}));
            }
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            if (!query.exec())
                return makeQueryError(QStringLiteral("List message user keywords"), query);

            std::vector<std::string> keywords;
            while (query.next())
            {
                auto keyword = query.value(0).toString().toStdString();
                if (!javelin::jmap::domain::hasStandardKeywordSemantics(keyword))
                    keywords.push_back(std::move(keyword));
            }
            return keywords;
        }

        [[nodiscard]] std::variant<std::vector<std::string>, DatabaseError>
        queryTagKeywords(const DatabaseReadView& connection, const std::string_view accountId)
        {
            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "SELECT d.keyword FROM mail_tag_definitions d "
                "WHERE d.account_id=:account_id AND NOT EXISTS(SELECT 1 FROM background_jobs j "
                "WHERE j.account_id=d.account_id AND j.kind='tag_deletion' "
                "AND j.status NOT IN ('failed','complete') "
                "AND json_extract(j.checkpoint_json,'$.keyword')=d.keyword COLLATE NOCASE) "
                "ORDER BY d.sort_order,d.display_name COLLATE NOCASE,d.keyword"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            if (!query.exec())
                return makeQueryError(QStringLiteral("List mail tag keywords"), query);

            std::vector<std::string> keywords;
            while (query.next())
                keywords.push_back(query.value(0).toString().toStdString());
            return keywords;
        }

        [[nodiscard]] std::optional<DatabaseError>
        attachMessageBodyPreviews(const DatabaseReadView& connection,
                                  const std::string_view accountId,
                                  std::vector<MessageListItem>& items)
        {
            if (items.empty())
                return std::nullopt;

            std::vector<std::string> emailIds;
            emailIds.reserve(items.size());
            std::unordered_map<std::string, MessageListItem*> itemsById;
            itemsById.reserve(items.size());
            for (auto& item : items)
            {
                item.bodyPreview.reset();
                emailIds.push_back(item.emailId);
                itemsById.emplace(item.emailId, &item);
            }

            std::string emailIdsJson;
            if (const auto error = glz::write_json(emailIds, emailIdsJson))
            {
                Q_UNUSED(error);
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message =
                        QStringLiteral("Serialize message ids for body preview lookup failed."),
                };
            }

            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "WITH requested AS MATERIALIZED (SELECT value AS email_id FROM "
                "json_each(:email_ids)) "
                "SELECT r.email_id,r.body_preview FROM requested q "
                "JOIN mail_vault_email_refs r ON r.account_id=:account_id AND "
                "r.email_id=q.email_id "
                "WHERE r.indexed_hash=r.content_hash AND r.body_preview IS NOT NULL"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_ids"), QString::fromStdString(emailIdsJson));
            if (!query.exec())
                return makeQueryError(QStringLiteral("Load message body previews"), query);

            while (query.next())
            {
                const auto found = itemsById.find(query.value(0).toString().toStdString());
                if (found != itemsById.end())
                    found->second->bodyPreview = query.value(1).toString().toStdString();
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError>
        attachMessageTagsOnly(const DatabaseReadView& connection, const std::string_view accountId,
                              std::vector<MessageListItem>& items)
        {
            if (items.empty())
                return std::nullopt;

            std::vector<std::string> emailIds;
            emailIds.reserve(items.size());
            std::unordered_map<std::string, MessageListItem*> itemsById;
            itemsById.reserve(items.size());
            for (auto& item : items)
            {
                item.tags.clear();
                emailIds.push_back(item.emailId);
                itemsById.emplace(item.emailId, &item);
            }

            std::string emailIdsJson;
            if (const auto error = glz::write_json(emailIds, emailIdsJson))
            {
                Q_UNUSED(error);
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Serialize message ids for tag lookup failed."),
                };
            }

            QSqlQuery query{connection.database()};
            query.prepare(QStringLiteral(
                "WITH requested AS MATERIALIZED ("
                " SELECT value AS email_id,CAST(key AS INTEGER) AS sort_index "
                " FROM json_each(:email_ids)) "
                "SELECT r.email_id,d.keyword,d.display_name,d.color FROM requested r "
                "JOIN email_keywords k ON k.account_id=:account_id AND k.email_id=r.email_id "
                "JOIN mail_tag_definitions d ON d.account_id=k.account_id "
                " AND d.keyword=k.keyword COLLATE NOCASE "
                "WHERE NOT EXISTS(SELECT 1 FROM background_jobs j "
                " WHERE j.account_id=d.account_id AND j.kind='tag_deletion' "
                " AND j.status NOT IN ('failed','complete') "
                " AND json_extract(j.checkpoint_json,'$.keyword')=d.keyword COLLATE NOCASE) "
                "ORDER BY r.sort_index,d.sort_order,d.display_name COLLATE NOCASE,d.keyword"));
            query.bindValue(QStringLiteral(":account_id"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":email_ids"), QString::fromStdString(emailIdsJson));
            if (!query.exec())
                return makeQueryError(QStringLiteral("Load message tags"), query);

            while (query.next())
            {
                const auto found = itemsById.find(query.value(0).toString().toStdString());
                if (found == itemsById.end())
                    continue;
                found->second->tags.push_back(MessageListTag{
                    .keyword = query.value(1).toString().toStdString(),
                    .displayName = query.value(2).toString(),
                    .color = query.value(3).toString(),
                });
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<DatabaseError>
        attachMessageTags(const DatabaseReadView& connection, const std::string_view accountId,
                          std::vector<MessageListItem>& items)
        {
            if (const auto error = attachMessageBodyPreviews(connection, accountId, items))
                return error;
            return attachMessageTagsOnly(connection, accountId, items);
        }

        [[nodiscard]] QString
        sortKeyExpression(const javelin::jmap::query::EmailListSortProperty property)
        {
            switch (property)
            {
            case javelin::jmap::query::EmailListSortProperty::ReceivedAt:
                return QStringLiteral("e.received_at");
            case javelin::jmap::query::EmailListSortProperty::SentAt:
                return QStringLiteral("COALESCE(e.sent_at, e.received_at)");
            case javelin::jmap::query::EmailListSortProperty::From:
                return QStringLiteral(
                    "LOWER(COALESCE((SELECT a.address FROM email_addresses a "
                    "WHERE a.account_id = e.account_id AND a.email_id = e.email_id "
                    "AND a.field_name = 'from' ORDER BY a.position LIMIT 1), ''))");
            case javelin::jmap::query::EmailListSortProperty::To:
                return QStringLiteral(
                    "LOWER(COALESCE((SELECT a.address FROM email_addresses a "
                    "WHERE a.account_id = e.account_id AND a.email_id = e.email_id "
                    "AND a.field_name = 'to' ORDER BY a.position LIMIT 1), ''))");
            case javelin::jmap::query::EmailListSortProperty::Subject:
                return QStringLiteral("LOWER(COALESCE(e.subject, ''))");
            case javelin::jmap::query::EmailListSortProperty::Size:
                return QStringLiteral("e.size");
            }

            return QStringLiteral("e.received_at");
        }

        [[nodiscard]] QueryWindowCoverage coverageFromValue(const QString& value)
        {
            if (value == QStringLiteral("server"))
                return QueryWindowCoverage::Server;
            if (value == QStringLiteral("locally_projected"))
                return QueryWindowCoverage::LocallyProjected;
            return QueryWindowCoverage::Stale;
        }

        [[nodiscard]] QueryWindowMaterialization materializationFromValue(const QString& value)
        {
            return value == QStringLiteral("complete") ? QueryWindowMaterialization::Complete
                                                       : QueryWindowMaterialization::Partial;
        }

        void bindSearchWindowKey(QSqlQuery& query, const std::string_view accountId,
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

        struct FilteredMailboxPredicate
        {
            QString sql;
            QString quickPattern;
            QString tagsJson;
            QString bodyIdsJson;
            std::size_t selectedTagCount = 0;
        };

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
                const auto keywordsResult = queryTagKeywords(connection, accountId);
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

        [[nodiscard]] SearchWindowResult readSearchWindow(const DatabaseReadView& connection,
                                                          const std::string_view accountId,
                                                          const std::string_view queryKey,
                                                          const std::size_t offset,
                                                          const std::size_t limit)
        {
            if (const auto error = connection.validate())
                return *error;

            QSqlQuery windowQuery{connection.database()};
            windowQuery.prepare(QStringLiteral(
                "SELECT position, returned_limit, total, query_state, coverage, "
                "materialization FROM search_windows WHERE account_id = :account_id AND "
                "query_key = :query_key AND window_offset = :offset AND window_limit = :limit"));
            bindSearchWindowKey(windowQuery, accountId, queryKey, offset, limit);
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

            QSqlQuery itemsQuery{connection.database()};
            itemsQuery.prepare(QStringLiteral(
                "SELECT email_id FROM search_window_items WHERE account_id = :account_id AND "
                "query_key = :query_key AND window_offset = :offset AND window_limit = :limit "
                "ORDER BY position"));
            bindSearchWindowKey(itemsQuery, accountId, queryKey, offset, limit);
            if (!itemsQuery.exec())
                return makeQueryError(QStringLiteral("Read search-window items"), itemsQuery);
            while (itemsQuery.next())
                record.emailIds.push_back(itemsQuery.value(0).toString().toStdString());
            return std::optional<SearchWindowRecord>{std::move(record)};
        }

        void bindMailboxWindowKey(QSqlQuery& query, const std::string_view accountId,
                                  const std::string_view queryKey,
                                  const std::size_t requestedOffset,
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

        [[nodiscard]] MailboxWindowResult readMailboxWindow(const DatabaseReadView& connection,
                                                            const std::string_view accountId,
                                                            const std::string_view queryKey,
                                                            const std::size_t requestedOffset,
                                                            const std::size_t requestedLimit)
        {
            if (const auto error = connection.validate())
                return *error;

            QSqlQuery windowQuery{connection.database()};
            windowQuery.prepare(QStringLiteral(
                "SELECT mailbox_id,position,returned_limit,total,query_state,coverage,"
                "materialization FROM mailbox_query_windows WHERE account_id=:account_id AND "
                "query_key=:query_key AND requested_offset=:requested_offset AND "
                "requested_limit=:requested_limit"));
            bindMailboxWindowKey(windowQuery, accountId, queryKey, requestedOffset, requestedLimit);
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

            QSqlQuery itemsQuery{connection.database()};
            itemsQuery.prepare(QStringLiteral(
                "SELECT email_id FROM mailbox_query_window_items WHERE account_id=:account_id "
                "AND query_key=:query_key AND requested_offset=:requested_offset AND "
                "requested_limit=:requested_limit ORDER BY position"));
            bindMailboxWindowKey(itemsQuery, accountId, queryKey, requestedOffset, requestedLimit);
            if (!itemsQuery.exec())
                return makeQueryError(QStringLiteral("Read mailbox-window items"), itemsQuery);
            while (itemsQuery.next())
                record.emailIds.push_back(itemsQuery.value(0).toString().toStdString());
            return std::optional<MailboxWindowRecord>{std::move(record)};
        }

    } // namespace

    QueryService::QueryService(DatabaseConnection& connection)
        : m_connection(connection), m_writeConnection(&connection)
    {
    }

    QueryService::QueryService(ReadOnlyDatabaseConnection& connection) : m_connection(connection)
    {
    }

    std::variant<std::vector<MailboxTreeItem>, DatabaseError>
    QueryService::listMailboxTree(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT m.mailbox_id, m.name, m.parent_mailbox_id, m.role, m.sort_order, "
            "m.total_emails, m.unread_emails, m.total_threads, m.unread_threads, "
            "m.is_subscribed, m.rights_json, "
            "EXISTS("
            "  SELECT 1 FROM mailboxes child "
            "  WHERE child.account_id = m.account_id AND child.parent_mailbox_id = m.mailbox_id"
            ") AS has_children "
            "FROM mailboxes m "
            "WHERE m.account_id = :account_id "
            "ORDER BY COALESCE(m.parent_mailbox_id, ''), m.sort_order, m.mailbox_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read mailbox tree"), query);
        }

        std::vector<MailboxTreeItem> items;
        while (query.next())
        {
            items.push_back(MailboxTreeItem{
                .id = query.value(0).toString().toStdString(),
                .name = query.value(1).toString().toStdString(),
                .parentId = query.value(2).isNull()
                                ? std::nullopt
                                : std::optional{query.value(2).toString().toStdString()},
                .role = query.value(3).isNull()
                            ? std::nullopt
                            : std::optional{query.value(3).toString().toStdString()},
                .sortOrder = query.value(4).toULongLong(),
                .totalEmails = query.value(5).toULongLong(),
                .unreadEmails = query.value(6).toULongLong(),
                .totalThreads = query.value(7).toULongLong(),
                .unreadThreads = query.value(8).toULongLong(),
                .isSubscribed = query.value(9).toInt() != 0,
                .myRights = deserializeMailboxRights(query.value(10).toString()),
                .hasChildren = query.value(11).toInt() != 0,
            });
        }

        return items;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError> QueryService::listMailboxMessages(
        const std::string_view accountId, const std::string_view mailboxId, const std::size_t limit,
        const std::size_t offset, javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = sortKeyExpression(sort.property);
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral(
                "WITH mailbox_email_ids AS MATERIALIZED ("
                "  SELECT em.email_id "
                "  FROM email_mailboxes em INDEXED BY idx_email_mailboxes_mailbox "
                "  WHERE em.account_id = :account_id AND em.mailbox_id = :mailbox_id"
                "), ranked_threads AS ("
                "  SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
                "         %2 AS sort_key, "
                "         ROW_NUMBER() OVER (PARTITION BY e.thread_id "
                "                            ORDER BY %2 %1, e.email_id %1) AS "
                "thread_rank, "
                "         (SELECT COUNT(mailbox_member.email_id) "
                "          FROM thread_email_members member "
                "          INNER JOIN email_mailboxes mailbox_member "
                "            ON mailbox_member.account_id=member.account_id "
                "            AND mailbox_member.email_id=member.email_id "
                "            AND mailbox_member.mailbox_id=:mailbox_id "
                "          WHERE member.account_id=e.account_id "
                "            AND member.thread_id=e.thread_id) AS cached_mailbox_count, "
                "         t.membership_freshness,t.member_count,"
                "         (SELECT COUNT(cached.email_id) FROM thread_email_members member "
                "          LEFT JOIN emails cached ON cached.account_id=member.account_id "
                "            AND cached.email_id=member.email_id "
                "          WHERE member.account_id=e.account_id "
                "            AND member.thread_id=e.thread_id) AS cached_global_count,"
                "         e.has_attachment,CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END "
                "           AS is_unread,"
                "         CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END AS is_flagged "
                "  FROM mailbox_email_ids me "
                "  CROSS JOIN emails e ON e.account_id = :account_id AND e.email_id = me.email_id "
                "  LEFT JOIN threads t ON t.account_id=e.account_id AND t.thread_id=e.thread_id "
                "  LEFT JOIN email_keywords seen ON seen.account_id = e.account_id "
                "       AND seen.email_id = e.email_id AND seen.keyword = '$seen' "
                "  LEFT JOIN email_keywords flagged ON flagged.account_id = e.account_id "
                "       AND flagged.email_id = e.email_id AND flagged.keyword = '$flagged' "
                ") "
                "SELECT rt.email_id, rt.thread_id, rt.subject, rt.preview, rt.received_at, "
                "rt.sent_at, "
                "CASE WHEN rt.membership_freshness='current' "
                " AND rt.member_count=rt.cached_global_count THEN rt.cached_mailbox_count "
                " ELSE NULL END,rt.member_count,rt.has_attachment,rt.is_unread,rt.is_flagged, "
                "EXISTS(SELECT 1 FROM email_mailboxes junk_membership "
                "INNER JOIN mailboxes junk_mailbox "
                "ON junk_mailbox.account_id=junk_membership.account_id "
                "AND junk_mailbox.mailbox_id=junk_membership.mailbox_id "
                "WHERE junk_membership.account_id=:account_id "
                "AND junk_membership.email_id=rt.email_id AND junk_mailbox.role='junk') "
                "AS is_junk, "
                "("
                "  SELECT a.display_name FROM email_addresses a "
                "  WHERE a.account_id = :account_id AND a.email_id = rt.email_id AND a.field_name "
                "= "
                "'from' "
                "  ORDER BY a.position LIMIT 1"
                ") AS from_name, "
                "("
                "  SELECT a.address FROM email_addresses a "
                "  WHERE a.account_id = :account_id AND a.email_id = rt.email_id AND a.field_name "
                "= "
                "'from' "
                "  ORDER BY a.position LIMIT 1"
                ") AS from_email "
                "FROM ranked_threads rt "
                "WHERE rt.thread_rank = 1 "
                "ORDER BY rt.sort_key %1, rt.email_id %1 "
                "LIMIT :limit OFFSET :offset")
                .arg(orderDirection, sortKey));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read mailbox message list"), query);
        }

        std::vector<MessageListItem> items;
        while (query.next())
        {
            MessageListItem item{
                .emailId = query.value(0).toString().toStdString(),
                .threadId = query.value(1).toString().toStdString(),
                .subject = query.value(2).isNull()
                               ? std::nullopt
                               : std::optional{query.value(2).toString().toStdString()},
                .preview = query.value(3).isNull()
                               ? std::nullopt
                               : std::optional{query.value(3).toString().toStdString()},
                .receivedAt = query.value(4).toString().toStdString(),
                .sentAt = query.value(5).isNull()
                              ? std::nullopt
                              : std::optional{query.value(5).toString().toStdString()},
                .mailboxThreadMessageCount = optionalCount(query.value(6)),
                .globalThreadMessageCount = optionalCount(query.value(7)),
                .hasAttachment = query.value(8).toInt() != 0,
                .isUnread = query.value(9).toInt() != 0,
                .isFlagged = query.value(10).toInt() != 0,
                .from = std::nullopt,
                .mailboxNames = {},
            };

            item.isJunk = query.value(11).toInt() != 0;
            if (!query.value(13).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(12).isNull()
                                ? std::nullopt
                                : std::optional{query.value(12).toString().toStdString()},
                    .email = query.value(13).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

        if (const auto error = attachMessageTags(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::optional<OfflineMailboxCoverage>, DatabaseError>
    QueryService::offlineMailboxCoverage(const std::string_view accountId,
                                         const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "WITH active_additions AS ("
            "  SELECT DISTINCT j.object_id AS email_id FROM mutation_journal j "
            "  INNER JOIN email_mailboxes em ON em.account_id=j.account_id "
            "    AND em.email_id=j.object_id AND em.mailbox_id=:mailbox_id "
            "  WHERE j.account_id=:account_id AND j.data_type='Email' "
            "    AND j.status IN ('pending','in_flight','unknown')"
            "), candidates AS ("
            "  SELECT m.email_id FROM offline_mailbox_scopes s "
            "  INNER JOIN offline_mailbox_membership m ON m.account_id=s.account_id "
            "    AND m.mailbox_id=s.mailbox_id AND m.generation=s.generation "
            "  INNER JOIN email_mailboxes em ON em.account_id=m.account_id "
            "    AND em.email_id=m.email_id AND em.mailbox_id=m.mailbox_id "
            "  WHERE s.account_id=:account_id AND s.mailbox_id=:mailbox_id "
            "  UNION SELECT email_id FROM active_additions"
            ") SELECT s.generation,s.status,COUNT(DISTINCT e.thread_id) "
            "FROM offline_mailbox_scopes s LEFT JOIN candidates c ON TRUE "
            "LEFT JOIN emails e ON e.account_id=s.account_id AND e.email_id=c.email_id "
            "WHERE s.account_id=:account_id AND s.mailbox_id=:mailbox_id AND s.desired=1 "
            "AND s.status IN ('enumerating','fetching','reconciling') "
            "GROUP BY s.generation,s.status"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read offline mailbox coverage"), query);
        if (!query.next())
            return std::optional<OfflineMailboxCoverage>{};
        return std::optional{OfflineMailboxCoverage{
            .generation = query.value(0).toULongLong(),
            .representativeCount = static_cast<std::size_t>(query.value(2).toULongLong()),
            .enumerationComplete = query.value(1).toString() != QStringLiteral("enumerating"),
        }};
    }

    std::variant<bool, DatabaseError>
    QueryService::offlineMailboxComplete(const std::string_view accountId,
                                         const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT EXISTS(SELECT 1 FROM offline_mailbox_scopes WHERE account_id=:account "
            "AND mailbox_id=:mailbox AND desired=1 AND status='complete' AND "
            "completed_generation=generation)"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(std::string{mailboxId}));
        if (!query.exec() || !query.next())
            return makeQueryError(QStringLiteral("Read complete offline mailbox status"), query);
        return query.value(0).toBool();
    }

    std::variant<std::optional<std::string>, DatabaseError>
    QueryService::completeOfflineMailboxQueryState(const std::string_view accountId,
                                                   const std::string_view mailboxId,
                                                   const std::string_view canonicalQueryKey) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT ss.state_token FROM offline_mailbox_scopes s INNER JOIN sync_state ss ON "
            "ss.account_id=s.account_id AND ss.object_type='EmailQuery' AND "
            "ss.query_key=:query_key WHERE s.account_id=:account AND s.mailbox_id=:mailbox AND "
            "s.desired=1 AND s.status='complete'"));
        query.bindValue(QStringLiteral(":query_key"),
                        QString::fromStdString(std::string{canonicalQueryKey}));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox"), QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read complete offline mailbox state"), query);
        if (!query.next())
            return std::optional<std::string>{};
        return std::optional<std::string>{query.value(0).toString().toStdString()};
    }

    std::variant<std::vector<std::string>, DatabaseError>
    QueryService::listOfflineMailboxRepresentativeIds(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::uint64_t generation, const std::size_t limit, const std::size_t offset,
        javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = sortKeyExpression(sort.property);
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("WITH candidates AS ("
                           "  SELECT m.email_id FROM offline_mailbox_membership m "
                           "  INNER JOIN email_mailboxes em ON em.account_id=m.account_id "
                           "    AND em.email_id=m.email_id AND em.mailbox_id=m.mailbox_id "
                           "  WHERE m.account_id=:account_id AND m.mailbox_id=:mailbox_id "
                           "    AND m.generation=:generation "
                           "  UNION SELECT DISTINCT j.object_id FROM mutation_journal j "
                           "  INNER JOIN email_mailboxes em ON em.account_id=j.account_id "
                           "    AND em.email_id=j.object_id AND em.mailbox_id=:mailbox_id "
                           "  WHERE j.account_id=:account_id AND j.data_type='Email' "
                           "    AND j.status IN ('pending','in_flight','unknown')"
                           "), ranked_threads AS ("
                           "  SELECT e.email_id,e.thread_id,%2 AS sort_key,"
                           "         ROW_NUMBER() OVER (PARTITION BY e.thread_id "
                           "           ORDER BY %2 %1,e.email_id %1) AS thread_rank "
                           "  FROM candidates c INNER JOIN emails e ON e.account_id=:account_id "
                           "    AND e.email_id=c.email_id"
                           ") SELECT email_id FROM ranked_threads WHERE thread_rank=1 "
                           "ORDER BY sort_key %1,email_id %1 LIMIT :limit OFFSET :offset")
                .arg(orderDirection, sortKey));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read offline mailbox representatives"), query);

        std::vector<std::string> ids;
        ids.reserve(limit);
        while (query.next())
            ids.push_back(query.value(0).toString().toStdString());
        return ids;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listOfflineMailboxMessages(const std::string_view accountId,
                                             const std::string_view mailboxId,
                                             const std::uint64_t generation,
                                             const std::size_t limit, const std::size_t offset,
                                             javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = sortKeyExpression(sort.property);
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral(
                "WITH scope AS ("
                "  SELECT status='complete' AS mailbox_complete FROM offline_mailbox_scopes "
                "  WHERE account_id=:account_id AND mailbox_id=:mailbox_id"
                "),candidates AS ("
                "  SELECT m.email_id FROM offline_mailbox_membership m "
                "  INNER JOIN email_mailboxes em ON em.account_id=m.account_id "
                "    AND em.email_id=m.email_id AND em.mailbox_id=m.mailbox_id "
                "  WHERE m.account_id=:account_id AND m.mailbox_id=:mailbox_id "
                "    AND m.generation=:generation "
                "  UNION SELECT DISTINCT j.object_id FROM mutation_journal j "
                "  INNER JOIN email_mailboxes em ON em.account_id=j.account_id "
                "    AND em.email_id=j.object_id AND em.mailbox_id=:mailbox_id "
                "  WHERE j.account_id=:account_id AND j.data_type='Email' "
                "    AND j.status IN ('pending','in_flight','unknown')"
                "), ranked_threads AS ("
                "  SELECT e.email_id,e.thread_id,e.subject,e.preview,e.received_at,e.sent_at,"
                "         %2 AS sort_key,"
                "         ROW_NUMBER() OVER (PARTITION BY e.thread_id "
                "           ORDER BY %2 %1,e.email_id %1) AS thread_rank,"
                "         COUNT(*) OVER (PARTITION BY e.thread_id) AS candidate_thread_count,"
                "         scope.mailbox_complete,t.membership_freshness,t.member_count,"
                "         (SELECT COUNT(cached.email_id) FROM thread_email_members member "
                "          LEFT JOIN emails cached ON cached.account_id=member.account_id "
                "            AND cached.email_id=member.email_id "
                "          WHERE member.account_id=e.account_id "
                "            AND member.thread_id=e.thread_id) AS cached_global_count,"
                "         (SELECT COUNT(mailbox_member.email_id) "
                "          FROM thread_email_members member "
                "          INNER JOIN email_mailboxes mailbox_member "
                "            ON mailbox_member.account_id=member.account_id "
                "            AND mailbox_member.email_id=member.email_id "
                "            AND mailbox_member.mailbox_id=:mailbox_id "
                "          WHERE member.account_id=e.account_id "
                "            AND member.thread_id=e.thread_id) AS normalized_mailbox_count,"
                "         e.has_attachment,"
                "         CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END AS is_unread,"
                "         CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END AS is_flagged "
                "  FROM candidates c CROSS JOIN scope "
                "  INNER JOIN emails e ON e.account_id=:account_id "
                "    AND e.email_id=c.email_id "
                "  LEFT JOIN threads t ON t.account_id=e.account_id AND t.thread_id=e.thread_id "
                "  LEFT JOIN email_keywords seen ON seen.account_id=e.account_id "
                "    AND seen.email_id=e.email_id AND seen.keyword='$seen' "
                "  LEFT JOIN email_keywords flagged ON flagged.account_id=e.account_id "
                "    AND flagged.email_id=e.email_id AND flagged.keyword='$flagged'"
                ") SELECT rt.email_id,rt.thread_id,rt.subject,rt.preview,rt.received_at,"
                "rt.sent_at,CASE WHEN rt.mailbox_complete THEN rt.candidate_thread_count "
                " WHEN rt.membership_freshness='current' "
                "  AND rt.member_count=rt.cached_global_count THEN rt.normalized_mailbox_count "
                " ELSE NULL END,rt.member_count,rt.has_attachment,"
                "rt.is_unread,rt.is_flagged,"
                "EXISTS(SELECT 1 FROM email_mailboxes junk_membership "
                "INNER JOIN mailboxes junk_mailbox "
                "ON junk_mailbox.account_id=junk_membership.account_id "
                "AND junk_mailbox.mailbox_id=junk_membership.mailbox_id "
                "WHERE junk_membership.account_id=:account_id "
                "AND junk_membership.email_id=rt.email_id AND junk_mailbox.role='junk'),"
                "(SELECT a.display_name FROM email_addresses a WHERE a.account_id=:account_id "
                " AND a.email_id=rt.email_id AND a.field_name='from' ORDER BY a.position LIMIT 1),"
                "(SELECT a.address FROM email_addresses a WHERE a.account_id=:account_id "
                " AND a.email_id=rt.email_id AND a.field_name='from' ORDER BY a.position LIMIT 1) "
                "FROM ranked_threads rt WHERE rt.thread_rank=1 "
                "ORDER BY rt.sort_key %1,rt.email_id %1 LIMIT :limit OFFSET :offset")
                .arg(orderDirection, sortKey));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":generation"), static_cast<qulonglong>(generation));
        query.bindValue(QStringLiteral(":limit"), static_cast<qulonglong>(limit));
        query.bindValue(QStringLiteral(":offset"), static_cast<qulonglong>(offset));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read offline mailbox page"), query);

        std::vector<MessageListItem> items;
        while (query.next())
        {
            MessageListItem item{
                .emailId = query.value(0).toString().toStdString(),
                .threadId = query.value(1).toString().toStdString(),
                .subject = query.value(2).isNull()
                               ? std::nullopt
                               : std::optional{query.value(2).toString().toStdString()},
                .preview = query.value(3).isNull()
                               ? std::nullopt
                               : std::optional{query.value(3).toString().toStdString()},
                .receivedAt = query.value(4).toString().toStdString(),
                .sentAt = query.value(5).isNull()
                              ? std::nullopt
                              : std::optional{query.value(5).toString().toStdString()},
                .mailboxThreadMessageCount = optionalCount(query.value(6)),
                .globalThreadMessageCount = optionalCount(query.value(7)),
                .hasAttachment = query.value(8).toInt() != 0,
                .isUnread = query.value(9).toInt() != 0,
                .isFlagged = query.value(10).toInt() != 0,
                .from = std::nullopt,
                .mailboxNames = {},
            };
            item.isJunk = query.value(11).toInt() != 0;
            if (!query.value(13).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(12).isNull()
                                ? std::nullopt
                                : std::optional{query.value(12).toString().toStdString()},
                    .email = query.value(13).toString().toStdString(),
                };
            }
            items.push_back(std::move(item));
        }
        if (const auto error = attachMessageTags(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listMessagesByEmailIds(const std::string_view accountId,
                                         const std::vector<std::string>& emailIds) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        if (emailIds.empty())
        {
            return std::vector<MessageListItem>{};
        }

        std::string emailIdsJson;
        if (const auto writeError = glz::write_json(emailIds, emailIdsJson))
        {
            Q_UNUSED(writeError);
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Serialize message id list for cache query failed."),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "WITH requested AS MATERIALIZED ("
            "  SELECT value AS email_id, CAST(key AS INTEGER) AS sort_index "
            "  FROM json_each(:email_ids_json)"
            ") "
            "SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
            "       NULL AS mailbox_thread_message_count, thread_record.member_count, "
            "       e.has_attachment, CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END, "
            "       CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END, "
            "       EXISTS(SELECT 1 FROM email_mailboxes junk_membership "
            "         INNER JOIN mailboxes junk_mailbox "
            "           ON junk_mailbox.account_id=junk_membership.account_id "
            "          AND junk_mailbox.mailbox_id=junk_membership.mailbox_id "
            "         WHERE junk_membership.account_id=:account_id "
            "           AND junk_membership.email_id=e.email_id "
            "           AND junk_mailbox.role='junk') AS is_junk, "
            "       ("
            "         SELECT a.display_name FROM email_addresses a "
            "         WHERE a.account_id = :account_id AND a.email_id = e.email_id "
            "           AND a.field_name = 'from' "
            "         ORDER BY a.position LIMIT 1"
            "       ) AS from_name, "
            "       ("
            "         SELECT a.address FROM email_addresses a "
            "         WHERE a.account_id = :account_id AND a.email_id = e.email_id "
            "           AND a.field_name = 'from' "
            "         ORDER BY a.position LIMIT 1"
            "       ) AS from_email, "
            "       ("
            "         SELECT json_group_array(mailbox_name) FROM ("
            "           SELECT m.name AS mailbox_name "
            "           FROM email_mailboxes em "
            "           INNER JOIN mailboxes m ON m.account_id = em.account_id "
            "                AND m.mailbox_id = em.mailbox_id "
            "           WHERE em.account_id = :account_id AND em.email_id = e.email_id "
            "           ORDER BY m.sort_order, m.name, m.mailbox_id"
            "         )"
            "       ) AS mailbox_names_json "
            "FROM requested r "
            "CROSS JOIN emails e ON e.account_id = :account_id AND e.email_id = r.email_id "
            "LEFT JOIN threads thread_record ON thread_record.account_id=e.account_id "
            "     AND thread_record.thread_id=e.thread_id "
            "LEFT JOIN email_keywords seen ON seen.account_id = e.account_id "
            "     AND seen.email_id = e.email_id AND seen.keyword = '$seen' "
            "LEFT JOIN email_keywords flagged ON flagged.account_id = e.account_id "
            "     AND flagged.email_id = e.email_id AND flagged.keyword = '$flagged' "
            "ORDER BY r.sort_index ASC"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_ids_json"), QString::fromStdString(emailIdsJson));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read message list by email ids"), query);
        }

        std::vector<MessageListItem> items;
        items.reserve(emailIds.size());
        while (query.next())
        {
            MessageListItem item{
                .emailId = query.value(0).toString().toStdString(),
                .threadId = query.value(1).toString().toStdString(),
                .subject = query.value(2).isNull()
                               ? std::nullopt
                               : std::optional{query.value(2).toString().toStdString()},
                .preview = query.value(3).isNull()
                               ? std::nullopt
                               : std::optional{query.value(3).toString().toStdString()},
                .receivedAt = query.value(4).toString().toStdString(),
                .sentAt = query.value(5).isNull()
                              ? std::nullopt
                              : std::optional{query.value(5).toString().toStdString()},
                .mailboxThreadMessageCount = optionalCount(query.value(6)),
                .globalThreadMessageCount = optionalCount(query.value(7)),
                .hasAttachment = query.value(8).toInt() != 0,
                .isUnread = query.value(9).toInt() != 0,
                .isFlagged = query.value(10).toInt() != 0,
                .from = std::nullopt,
                .mailboxNames = {},
            };

            item.isJunk = query.value(11).toInt() != 0;
            if (!query.value(13).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(12).isNull()
                                ? std::nullopt
                                : std::optional{query.value(12).toString().toStdString()},
                    .email = query.value(13).toString().toStdString(),
                };
            }

            if (const auto mailboxNamesJson = query.value(14).toString().toStdString();
                !mailboxNamesJson.empty())
            {
                static_cast<void>(glz::read_json(item.mailboxNames, mailboxNamesJson));
            }

            items.push_back(std::move(item));
        }

        if (const auto error = attachMessageTags(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::optional<MessageListItem>, DatabaseError>
    QueryService::findMailboxMessage(const std::string_view accountId,
                                     const std::string_view mailboxId,
                                     const std::string_view emailId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
            "       CASE WHEN t.membership_freshness='current' AND t.member_count=("
            "         SELECT COUNT(cached.email_id) FROM thread_email_members thread_member "
            "         LEFT JOIN emails cached ON cached.account_id=thread_member.account_id "
            "           AND cached.email_id=thread_member.email_id "
            "         WHERE thread_member.account_id=e.account_id "
            "           AND thread_member.thread_id=e.thread_id"
            "       ) THEN (SELECT COUNT(*) FROM thread_email_members thread_member "
            "          INNER JOIN email_mailboxes thread_membership "
            "            ON thread_membership.account_id=thread_member.account_id "
            "           AND thread_membership.email_id=thread_member.email_id "
            "           AND thread_membership.mailbox_id=:mailbox_id "
            "          WHERE thread_member.account_id=e.account_id "
            "            AND thread_member.thread_id=e.thread_id) ELSE NULL END,"
            "       t.member_count, "
            "       e.has_attachment, "
            "       CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END AS is_unread, "
            "       CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END AS is_flagged, "
            "       EXISTS(SELECT 1 FROM email_mailboxes junk_membership "
            "         INNER JOIN mailboxes junk_mailbox "
            "           ON junk_mailbox.account_id=junk_membership.account_id "
            "          AND junk_mailbox.mailbox_id=junk_membership.mailbox_id "
            "         WHERE junk_membership.account_id=:account_id "
            "           AND junk_membership.email_id=e.email_id "
            "           AND junk_mailbox.role='junk') AS is_junk, "
            "       (SELECT a.display_name FROM email_addresses a "
            "         WHERE a.account_id=:account_id AND a.email_id=e.email_id "
            "           AND a.field_name='from' ORDER BY a.position LIMIT 1) AS from_name, "
            "       (SELECT a.address FROM email_addresses a "
            "         WHERE a.account_id=:account_id AND a.email_id=e.email_id "
            "           AND a.field_name='from' ORDER BY a.position LIMIT 1) AS from_email "
            "FROM emails e "
            "LEFT JOIN threads t ON t.account_id=e.account_id AND t.thread_id=e.thread_id "
            "INNER JOIN email_mailboxes selected_membership "
            "  ON selected_membership.account_id=e.account_id "
            " AND selected_membership.email_id=e.email_id "
            " AND selected_membership.mailbox_id=:mailbox_id "
            "LEFT JOIN email_keywords seen ON seen.account_id=e.account_id "
            " AND seen.email_id=e.email_id AND seen.keyword='$seen' "
            "LEFT JOIN email_keywords flagged ON flagged.account_id=e.account_id "
            " AND flagged.email_id=e.email_id AND flagged.keyword='$flagged' "
            "WHERE e.account_id=:account_id AND e.email_id=:email_id LIMIT 1"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Read mailbox message"), query);
        if (!query.next())
            return std::optional<MessageListItem>{std::nullopt};

        MessageListItem item{
            .emailId = query.value(0).toString().toStdString(),
            .threadId = query.value(1).toString().toStdString(),
            .subject = query.value(2).isNull()
                           ? std::nullopt
                           : std::optional{query.value(2).toString().toStdString()},
            .preview = query.value(3).isNull()
                           ? std::nullopt
                           : std::optional{query.value(3).toString().toStdString()},
            .receivedAt = query.value(4).toString().toStdString(),
            .sentAt = query.value(5).isNull()
                          ? std::nullopt
                          : std::optional{query.value(5).toString().toStdString()},
            .mailboxThreadMessageCount = optionalCount(query.value(6)),
            .globalThreadMessageCount = optionalCount(query.value(7)),
            .hasAttachment = query.value(8).toInt() != 0,
            .isUnread = query.value(9).toInt() != 0,
            .isFlagged = query.value(10).toInt() != 0,
            .from = std::nullopt,
            .mailboxNames = {},
        };
        item.isJunk = query.value(11).toInt() != 0;
        if (!query.value(13).isNull())
        {
            item.from = javelin::jmap::domain::EmailAddress{
                .name = query.value(12).isNull()
                            ? std::nullopt
                            : std::optional{query.value(12).toString().toStdString()},
                .email = query.value(13).toString().toStdString(),
            };
        }

        std::vector<MessageListItem> items;
        items.push_back(std::move(item));
        if (const auto error = attachMessageTags(m_connection, accountId, items))
            return *error;
        return std::optional<MessageListItem>{std::move(items.front())};
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listMailboxWindowMessagesByEmailIds(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::vector<std::string>& emailIds, javelin::jmap::query::EmailListSort sort) const
    {
        Q_UNUSED(sort);
        if (const auto error = m_connection.validate())
            return *error;
        if (emailIds.empty())
            return std::vector<MessageListItem>{};

        std::string emailIdsJson;
        if (const auto writeError = glz::write_json(emailIds, emailIdsJson))
        {
            Q_UNUSED(writeError);
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Serialize mailbox-window ids failed."),
            };
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "WITH requested AS MATERIALIZED ("
            "  SELECT value AS email_id,CAST(key AS INTEGER) AS window_position "
            "  FROM json_each(:email_ids_json)"
            ") SELECT r.email_id,"
            "  CASE WHEN t.membership_freshness='current' AND t.member_count=("
            "    SELECT COUNT(cached.email_id) FROM thread_email_members member "
            "    LEFT JOIN emails cached ON cached.account_id=member.account_id "
            "      AND cached.email_id=member.email_id "
            "    WHERE member.account_id=e.account_id AND member.thread_id=e.thread_id"
            "  ) THEN ("
            "    SELECT COUNT(mailbox_member.email_id) FROM thread_email_members member "
            "    INNER JOIN email_mailboxes mailbox_member "
            "      ON mailbox_member.account_id=member.account_id "
            "      AND mailbox_member.email_id=member.email_id "
            "      AND mailbox_member.mailbox_id=:mailbox_id "
            "    WHERE member.account_id=e.account_id AND member.thread_id=e.thread_id"
            "  ) ELSE NULL END "
            "FROM requested r "
            "CROSS JOIN emails e ON e.account_id=:account_id AND e.email_id=r.email_id "
            "CROSS JOIN email_mailboxes selected_membership "
            "  ON selected_membership.account_id=e.account_id "
            "  AND selected_membership.email_id=e.email_id "
            "  AND selected_membership.mailbox_id=:mailbox_id "
            "LEFT JOIN threads t ON t.account_id=e.account_id AND t.thread_id=e.thread_id "
            "ORDER BY r.window_position"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":email_ids_json"), QString::fromStdString(emailIdsJson));
        if (!query.exec())
            return makeQueryError(QStringLiteral("Project mailbox-window membership"), query);

        std::vector<std::string> projectedIds;
        std::vector<std::optional<std::uint64_t>> mailboxThreadMessageCounts;
        projectedIds.reserve(emailIds.size());
        mailboxThreadMessageCounts.reserve(emailIds.size());
        while (query.next())
        {
            projectedIds.push_back(query.value(0).toString().toStdString());
            mailboxThreadMessageCounts.push_back(optionalCount(query.value(1)));
        }

        auto messagesResult = listMessagesByEmailIds(accountId, projectedIds);
        auto* messages = std::get_if<std::vector<MessageListItem>>(&messagesResult);
        if (messages == nullptr)
            return std::get<DatabaseError>(messagesResult);
        if (messages->size() != mailboxThreadMessageCounts.size())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message =
                    QStringLiteral("Mailbox-window projection returned inconsistent row counts."),
            };
        }
        for (std::size_t index = 0; index < messages->size(); ++index)
            (*messages)[index].mailboxThreadMessageCount = mailboxThreadMessageCounts[index];
        return std::move(*messages);
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listSortedSearchMessagesByEmailIds(
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
        const auto sortKey = sortKeyExpression(sort.property);
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
        return listMessagesByEmailIds(accountId, sortedIds);
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::searchCachedMessageText(const std::string_view accountId,
                                          const std::string_view text, const std::size_t limit,
                                          const std::size_t offset) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }
        if (text.empty() || limit == 0)
        {
            return std::vector<MessageListItem>{};
        }

        MailSearchIndex index{m_connection};
        const auto indexedIds = index.search(accountId, text, (limit + offset) * 4);
        const auto* emailIds = std::get_if<std::vector<std::string>>(&indexedIds);
        if (emailIds == nullptr)
            return std::get<DatabaseError>(indexedIds);
        const auto itemResult = listMessagesByEmailIds(accountId, *emailIds);
        if (const auto* error = std::get_if<DatabaseError>(&itemResult))
        {
            return *error;
        }

        std::vector<MessageListItem> results;
        std::unordered_set<std::string> threadIds;
        for (const auto& item : std::get<std::vector<MessageListItem>>(itemResult))
        {
            if (threadIds.insert(item.threadId).second)
            {
                if (threadIds.size() > offset)
                {
                    results.push_back(item);
                }
                if (results.size() == limit)
                {
                    break;
                }
            }
        }
        return results;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::searchAllCachedMessageText(const std::string_view accountId,
                                             const std::string_view text,
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
        return listSortedSearchMessagesByEmailIds(accountId, *emailIds, sort);
    }

    QString QueryService::databasePath() const
    {
        return m_connection.database().databaseName();
    }

    std::variant<std::uint64_t, DatabaseError> QueryService::dataVersion() const
    {
        return m_connection.dataVersion();
    }

    std::variant<std::optional<SearchWindowPage>, DatabaseError>
    QueryService::loadSearchWindow(const std::string_view accountId,
                                   const std::string_view queryKey, const std::size_t offset,
                                   const std::size_t limit) const
    {
        const auto windowResult =
            m_writeConnection != nullptr
                ? SearchWindowRepository{*m_writeConnection}.find(accountId, queryKey, offset,
                                                                  limit)
                : readSearchWindow(m_connection, accountId, queryKey, offset, limit);
        const auto* window = std::get_if<std::optional<SearchWindowRecord>>(&windowResult);
        if (window == nullptr)
        {
            return std::get<DatabaseError>(windowResult);
        }
        if (!window->has_value())
        {
            return std::optional<SearchWindowPage>{std::nullopt};
        }

        const auto messagesResult = listMessagesByEmailIds(accountId, (*window)->emailIds);
        const auto* messages = std::get_if<std::vector<MessageListItem>>(&messagesResult);
        if (messages == nullptr)
        {
            return std::get<DatabaseError>(messagesResult);
        }

        return std::optional<SearchWindowPage>{SearchWindowPage{
            .offset = (*window)->offset,
            .limit = (*window)->limit,
            .position = (*window)->position,
            .returnedLimit = (*window)->returnedLimit,
            .total = (*window)->total,
            .queryState = (*window)->queryState,
            .coverage = (*window)->coverage,
            .materialization = (*window)->materialization,
            .items = *messages,
        }};
    }

    std::optional<DatabaseError>
    QueryService::eraseSearchWindows(const std::string_view accountId,
                                     const std::string_view queryKey) const
    {
        if (m_writeConnection == nullptr)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Erase search windows requires daemon cache access"),
            };
        }
        SearchWindowRepository repository{*m_writeConnection};
        return repository.eraseQuery(accountId, queryKey);
    }

    std::variant<std::optional<MailboxWindowPage>, DatabaseError> QueryService::loadMailboxWindow(
        const std::string_view accountId, const std::string_view queryKey,
        const std::size_t requestedOffset, const std::size_t requestedLimit,
        javelin::jmap::query::EmailListSort sort) const
    {
        QElapsedTimer timer;
        timer.start();
        const auto windowResult = m_writeConnection != nullptr
                                      ? MailboxWindowRepository{*m_writeConnection}.find(
                                            accountId, queryKey, requestedOffset, requestedLimit)
                                      : readMailboxWindow(m_connection, accountId, queryKey,
                                                          requestedOffset, requestedLimit);
        const auto windowMilliseconds = timer.restart();
        const auto* window = std::get_if<std::optional<MailboxWindowRecord>>(&windowResult);
        if (window == nullptr)
            return std::get<DatabaseError>(windowResult);
        if (!window->has_value())
            return std::optional<MailboxWindowPage>{std::nullopt};

        if ((*window)->coverage == QueryWindowCoverage::LocallyProjected)
        {
            const auto projectedResult = listMailboxMessages(accountId, (*window)->mailboxId,
                                                             requestedLimit, requestedOffset, sort);
            const auto* projected = std::get_if<std::vector<MessageListItem>>(&projectedResult);
            if (projected == nullptr)
                return std::get<DatabaseError>(projectedResult);
            const auto totalResult = countMailboxMessages(accountId, (*window)->mailboxId);
            const auto* total = std::get_if<std::size_t>(&totalResult);
            if (total == nullptr)
                return std::get<DatabaseError>(totalResult);
            return std::optional<MailboxWindowPage>{MailboxWindowPage{
                .requestedOffset = requestedOffset,
                .requestedLimit = requestedLimit,
                .position = requestedOffset,
                .returnedLimit = projected->size(),
                .total = *total,
                .queryState = (*window)->queryState,
                .coverage = QueryWindowCoverage::LocallyProjected,
                .materialization = (*window)->materialization,
                .items = *projected,
            }};
        }

        const auto messagesResult = listMailboxWindowMessagesByEmailIds(
            accountId, (*window)->mailboxId, (*window)->emailIds, sort);
        const auto messageMilliseconds = timer.elapsed();
        if (windowMilliseconds + messageMilliseconds >= 50)
        {
            qCWarning(logQueryPerformance).noquote()
                << "Slow mailbox window load" << QString::fromStdString(std::string{accountId})
                << QString::fromStdString((*window)->mailboxId) << "windowMs" << windowMilliseconds
                << "messagesMs" << messageMilliseconds << "items"
                << static_cast<qulonglong>((*window)->emailIds.size());
        }
        const auto* messages = std::get_if<std::vector<MessageListItem>>(&messagesResult);
        if (messages == nullptr)
            return std::get<DatabaseError>(messagesResult);

        return std::optional<MailboxWindowPage>{MailboxWindowPage{
            .requestedOffset = (*window)->requestedOffset,
            .requestedLimit = (*window)->requestedLimit,
            .position = (*window)->position,
            .returnedLimit = (*window)->returnedLimit,
            .total = (*window)->total,
            .queryState = (*window)->queryState,
            .coverage = (*window)->coverage,
            .materialization = (*window)->materialization,
            .items = *messages,
        }};
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listThreadMessages(const std::string_view accountId,
                                     const std::string_view threadId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
            "       NULL AS mailbox_thread_message_count, e.has_attachment, "
            "       CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END AS is_unread, "
            "       CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END AS is_flagged, "
            "       EXISTS(SELECT 1 FROM email_mailboxes junk_membership "
            "         INNER JOIN mailboxes junk_mailbox "
            "           ON junk_mailbox.account_id=junk_membership.account_id "
            "          AND junk_mailbox.mailbox_id=junk_membership.mailbox_id "
            "         WHERE junk_membership.account_id=:account_id "
            "           AND junk_membership.email_id=e.email_id "
            "           AND junk_mailbox.role='junk') AS is_junk, "
            "       ("
            "         SELECT a.display_name FROM email_addresses a "
            "         WHERE a.account_id = :account_id AND a.email_id = e.email_id "
            "           AND a.field_name = 'from' "
            "         ORDER BY a.position LIMIT 1"
            "       ) AS from_name, "
            "       ("
            "         SELECT a.address FROM email_addresses a "
            "         WHERE a.account_id = :account_id AND a.email_id = e.email_id "
            "           AND a.field_name = 'from' "
            "         ORDER BY a.position LIMIT 1"
            "       ) AS from_email "
            "FROM thread_email_members thread_email "
            "INNER JOIN emails e ON e.account_id = thread_email.account_id "
            "     AND e.email_id = thread_email.email_id "
            "LEFT JOIN email_keywords seen ON seen.account_id = e.account_id "
            "     AND seen.email_id = e.email_id AND seen.keyword = '$seen' "
            "LEFT JOIN email_keywords flagged ON flagged.account_id = e.account_id "
            "     AND flagged.email_id = e.email_id AND flagged.keyword = '$flagged' "
            "WHERE thread_email.account_id = :account_id "
            "  AND thread_email.thread_id = :thread_id "
            "ORDER BY thread_email.position ASC"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":thread_id"),
                        QString::fromStdString(std::string{threadId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read thread messages"), query);
        }

        std::vector<MessageListItem> items;
        while (query.next())
        {
            MessageListItem item{
                .emailId = query.value(0).toString().toStdString(),
                .threadId = query.value(1).toString().toStdString(),
                .subject = query.value(2).isNull()
                               ? std::nullopt
                               : std::optional{query.value(2).toString().toStdString()},
                .preview = query.value(3).isNull()
                               ? std::nullopt
                               : std::optional{query.value(3).toString().toStdString()},
                .receivedAt = query.value(4).toString().toStdString(),
                .sentAt = query.value(5).isNull()
                              ? std::nullopt
                              : std::optional{query.value(5).toString().toStdString()},
                .mailboxThreadMessageCount = optionalCount(query.value(6)),
                .globalThreadMessageCount = std::nullopt,
                .hasAttachment = query.value(7).toInt() != 0,
                .isUnread = query.value(8).toInt() != 0,
                .isFlagged = query.value(9).toInt() != 0,
                .from = std::nullopt,
                .mailboxNames = {},
            };

            item.isJunk = query.value(10).toInt() != 0;
            if (!query.value(12).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(11).isNull()
                                ? std::nullopt
                                : std::optional{query.value(11).toString().toStdString()},
                    .email = query.value(12).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

        if (const auto error = attachMessageTags(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listMailboxThreadMessages(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::string_view threadId, const MailboxThreadMembershipSource membershipSource) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        const auto select = QStringLiteral(
            "SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
            "       NULL AS mailbox_thread_message_count, e.has_attachment, "
            "       CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END AS is_unread, "
            "       CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END AS is_flagged, "
            "       EXISTS(SELECT 1 FROM email_mailboxes junk_membership "
            "         INNER JOIN mailboxes junk_mailbox "
            "           ON junk_mailbox.account_id=junk_membership.account_id "
            "          AND junk_mailbox.mailbox_id=junk_membership.mailbox_id "
            "         WHERE junk_membership.account_id=:account_id "
            "           AND junk_membership.email_id=e.email_id "
            "           AND junk_mailbox.role='junk') AS is_junk, "
            "       ("
            "         SELECT a.display_name FROM email_addresses a "
            "         WHERE a.account_id = :account_id AND a.email_id = e.email_id "
            "           AND a.field_name = 'from' "
            "         ORDER BY a.position LIMIT 1"
            "       ) AS from_name, "
            "       ("
            "         SELECT a.address FROM email_addresses a "
            "         WHERE a.account_id = :account_id AND a.email_id = e.email_id "
            "           AND a.field_name = 'from' "
            "         ORDER BY a.position LIMIT 1"
            "       ) AS from_email ");
        const auto source =
            membershipSource == MailboxThreadMembershipSource::CompleteOfflineMailbox
                ? QStringLiteral(
                      "FROM emails e "
                      "INNER JOIN email_mailboxes em ON em.account_id = e.account_id "
                      "     AND em.email_id = e.email_id AND em.mailbox_id = :mailbox_id ")
                : QStringLiteral(
                      "FROM thread_email_members thread_email "
                      "INNER JOIN emails e ON e.account_id = thread_email.account_id "
                      "     AND e.email_id = thread_email.email_id "
                      "INNER JOIN email_mailboxes em ON em.account_id = e.account_id "
                      "     AND em.email_id = e.email_id AND em.mailbox_id = :mailbox_id ");
        const auto joins = QStringLiteral(
            "LEFT JOIN email_keywords seen ON seen.account_id = e.account_id "
            "     AND seen.email_id = e.email_id AND seen.keyword = '$seen' "
            "LEFT JOIN email_keywords flagged ON flagged.account_id = e.account_id "
            "     AND flagged.email_id = e.email_id AND flagged.keyword = '$flagged' ");
        const auto predicateAndOrder =
            membershipSource == MailboxThreadMembershipSource::CompleteOfflineMailbox
                ? QStringLiteral("WHERE e.account_id = :account_id AND e.thread_id = :thread_id "
                                 "ORDER BY e.received_at ASC,e.email_id ASC")
                : QStringLiteral("WHERE thread_email.account_id = :account_id "
                                 "  AND thread_email.thread_id = :thread_id "
                                 "ORDER BY thread_email.position ASC");
        query.prepare(select + source + joins + predicateAndOrder);
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        query.bindValue(QStringLiteral(":thread_id"),
                        QString::fromStdString(std::string{threadId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Read mailbox thread messages"), query);
        }

        std::vector<MessageListItem> items;
        while (query.next())
        {
            MessageListItem item{
                .emailId = query.value(0).toString().toStdString(),
                .threadId = query.value(1).toString().toStdString(),
                .subject = query.value(2).isNull()
                               ? std::nullopt
                               : std::optional{query.value(2).toString().toStdString()},
                .preview = query.value(3).isNull()
                               ? std::nullopt
                               : std::optional{query.value(3).toString().toStdString()},
                .receivedAt = query.value(4).toString().toStdString(),
                .sentAt = query.value(5).isNull()
                              ? std::nullopt
                              : std::optional{query.value(5).toString().toStdString()},
                .mailboxThreadMessageCount = optionalCount(query.value(6)),
                .globalThreadMessageCount = std::nullopt,
                .hasAttachment = query.value(7).toInt() != 0,
                .isUnread = query.value(8).toInt() != 0,
                .isFlagged = query.value(9).toInt() != 0,
                .from = std::nullopt,
                .mailboxNames = {},
            };

            item.isJunk = query.value(10).toInt() != 0;
            if (!query.value(12).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(11).isNull()
                                ? std::nullopt
                                : std::optional{query.value(11).toString().toStdString()},
                    .email = query.value(12).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

        if (const auto error = attachMessageTags(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::size_t, DatabaseError>
    QueryService::countMailboxMessages(const std::string_view accountId,
                                       const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("WITH mailbox_email_ids AS MATERIALIZED ("
                           "  SELECT em.email_id "
                           "  FROM email_mailboxes em INDEXED BY idx_email_mailboxes_mailbox "
                           "  WHERE em.account_id = :account_id AND em.mailbox_id = :mailbox_id"
                           ") "
                           "SELECT COUNT(DISTINCT e.thread_id) "
                           "FROM mailbox_email_ids me "
                           "CROSS JOIN emails e ON e.account_id = :account_id "
                           "     AND e.email_id = me.email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Count mailbox message list"), query);
        }

        if (!query.next())
        {
            return static_cast<std::size_t>(0);
        }

        return static_cast<std::size_t>(query.value(0).toULongLong());
    }

    std::variant<std::vector<std::string>, DatabaseError>
    QueryService::listUserKeywords(const std::string_view accountId,
                                   const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        return queryUserKeywords(m_connection, accountId, mailboxId);
    }

    std::variant<std::vector<std::string>, DatabaseError>
    QueryService::listTagKeywords(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        return queryTagKeywords(m_connection, accountId);
    }

    std::variant<std::vector<EmailKeywordMembership>, DatabaseError>
    QueryService::listEmailKeywordMemberships(const std::string_view accountId,
                                              const std::vector<std::string>& emailIds) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (emailIds.empty())
            return std::vector<EmailKeywordMembership>{};

        std::string idsJson;
        if (const auto error = glz::write_json(emailIds, idsJson))
        {
            Q_UNUSED(error);
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Serialize email ids for keyword lookup failed."),
            };
        }
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("WITH selected(email_id) AS (SELECT value FROM json_each(:ids)) "
                           "SELECT s.email_id,k.keyword FROM selected s LEFT JOIN email_keywords k "
                           "ON k.account_id=:account_id AND k.email_id=s.email_id "
                           "ORDER BY s.email_id,k.keyword COLLATE NOCASE"));
        query.bindValue(QStringLiteral(":ids"), QString::fromStdString(idsJson));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List email keyword memberships"), query);

        std::unordered_map<std::string, std::vector<std::string>> byEmail;
        byEmail.reserve(emailIds.size());
        while (query.next())
        {
            auto& keywords = byEmail[query.value(0).toString().toStdString()];
            if (!query.value(1).isNull())
                keywords.push_back(query.value(1).toString().toStdString());
        }
        std::vector<EmailKeywordMembership> memberships;
        memberships.reserve(emailIds.size());
        for (const auto& emailId : emailIds)
        {
            auto found = byEmail.find(emailId);
            memberships.push_back(EmailKeywordMembership{
                .emailId = emailId,
                .keywords =
                    found == byEmail.end() ? std::vector<std::string>{} : std::move(found->second),
            });
        }
        return memberships;
    }

    std::variant<std::vector<TagDefinition>, DatabaseError>
    QueryService::listTagDefinitions(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT d.keyword,d.display_name,d.color,d.sort_order FROM mail_tag_definitions d "
            "WHERE d.account_id=:account_id AND NOT EXISTS(SELECT 1 FROM background_jobs j "
            "WHERE j.account_id=d.account_id AND j.kind='tag_deletion' "
            "AND j.status NOT IN ('failed','complete') "
            "AND json_extract(j.checkpoint_json,'$.keyword')=d.keyword COLLATE NOCASE) "
            "ORDER BY d.sort_order,d.display_name COLLATE NOCASE,d.keyword"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List mail tag definitions"), query);

        std::vector<TagDefinition> definitions;
        while (query.next())
        {
            definitions.push_back(TagDefinition{
                .accountId = std::string{accountId},
                .keyword = query.value(0).toString().toStdString(),
                .displayName = query.value(1).toString(),
                .color = query.value(2).toString(),
                .sortOrder = query.value(3).toInt(),
            });
        }
        return definitions;
    }

    std::variant<std::vector<std::string>, DatabaseError>
    QueryService::listContactEmailAddresses() const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT DISTINCT address FROM contact_emails ORDER BY normalized_address"));
        if (!query.exec())
            return makeQueryError(QStringLiteral("List contact email addresses"), query);
        std::vector<std::string> addresses;
        while (query.next())
            addresses.push_back(query.value(0).toString().toStdString());
        return addresses;
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listFilteredMailboxMessages(
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
        const auto sortKey = sortKeyExpression(sort.property);
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
        return listMessagesByEmailIds(accountId, emailIds);
    }

    std::variant<std::size_t, DatabaseError> QueryService::countFilteredMailboxMessages(
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

    std::variant<std::size_t, DatabaseError>
    QueryService::countUnreadMailboxEmails(const std::string_view accountId,
                                           const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT COUNT(*) "
            "FROM email_mailboxes em "
            "WHERE em.account_id = :account_id AND em.mailbox_id = :mailbox_id "
            "  AND NOT EXISTS ("
            "    SELECT 1 FROM email_keywords seen "
            "    WHERE seen.account_id = em.account_id AND seen.email_id = em.email_id "
            "      AND seen.keyword = '$seen'"
            "  )"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":mailbox_id"),
                        QString::fromStdString(std::string{mailboxId}));
        if (!query.exec())
        {
            return makeQueryError(QStringLiteral("Count unread mailbox emails"), query);
        }

        if (!query.next())
        {
            return static_cast<std::size_t>(0);
        }
        return static_cast<std::size_t>(query.value(0).toULongLong());
    }

} // namespace javelin::jmap::cache
