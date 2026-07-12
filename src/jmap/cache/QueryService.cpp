#include "jmap/cache/QueryService.h"

#include "jmap/cache/SearchWindowRepository.h"

#include <glaze/glaze.hpp>

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
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
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

    } // namespace

    QueryService::QueryService(DatabaseConnection& connection) : m_connection(connection)
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
            "m.is_subscribed, "
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
                .hasChildren = query.value(10).toInt() != 0,
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
                "), mailbox_threads AS MATERIALIZED ("
                "  SELECT DISTINCT e.thread_id "
                "  FROM mailbox_email_ids me "
                "  CROSS JOIN emails e ON e.account_id = :account_id AND e.email_id = "
                "me.email_id"
                "), ranked_threads AS ("
                "  SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
                "         %2 AS sort_key, "
                "         ROW_NUMBER() OVER (PARTITION BY e.thread_id "
                "                            ORDER BY %2 %1, e.email_id %1) AS "
                "thread_rank, "
                "         COUNT(*) OVER (PARTITION BY e.thread_id) AS thread_message_count, "
                "         MAX(e.has_attachment) OVER (PARTITION BY e.thread_id) AS "
                "thread_has_attachment, "
                "         MAX(CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END) OVER "
                "             (PARTITION BY e.thread_id) AS thread_has_unread, "
                "         MAX(CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END) OVER "
                "             (PARTITION BY e.thread_id) AS thread_has_flagged "
                "  FROM mailbox_threads mt "
                "  CROSS JOIN emails e INDEXED BY idx_emails_thread "
                "       ON e.account_id = :account_id AND e.thread_id = mt.thread_id "
                "  LEFT JOIN email_keywords seen ON seen.account_id = e.account_id "
                "       AND seen.email_id = e.email_id AND seen.keyword = '$seen' "
                "  LEFT JOIN email_keywords flagged ON flagged.account_id = e.account_id "
                "       AND flagged.email_id = e.email_id AND flagged.keyword = '$flagged' "
                ") "
                "SELECT rt.email_id, rt.thread_id, rt.subject, rt.preview, rt.received_at, "
                "rt.sent_at, "
                "rt.thread_message_count, rt.thread_has_attachment, rt.thread_has_unread, "
                "rt.thread_has_flagged, "
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
                .threadMessageCount = query.value(6).toULongLong(),
                .hasAttachment = query.value(7).toInt() != 0,
                .isUnread = query.value(8).toInt() != 0,
                .isFlagged = query.value(9).toInt() != 0,
                .from = std::nullopt,
            };

            if (!query.value(11).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(10).isNull()
                                ? std::nullopt
                                : std::optional{query.value(10).toString().toStdString()},
                    .email = query.value(11).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

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
            "WITH requested AS ("
            "  SELECT value AS email_id, CAST(key AS INTEGER) AS sort_index "
            "  FROM json_each(:email_ids_json)"
            ") "
            "SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
            "       COALESCE(thread_size.thread_message_count, 1) AS thread_message_count, "
            "       COALESCE(thread_flags.thread_has_attachment, e.has_attachment) AS "
            "thread_has_attachment, "
            "       COALESCE(thread_flags.thread_has_unread, CASE WHEN seen.email_id IS NULL THEN "
            "1 "
            "ELSE 0 END) AS thread_has_unread, "
            "       COALESCE(thread_flags.thread_has_flagged, CASE WHEN flagged.email_id IS NULL "
            "THEN 0 ELSE 1 END) AS thread_has_flagged, "
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
            "FROM requested r "
            "INNER JOIN emails e ON e.account_id = :account_id AND e.email_id = r.email_id "
            "LEFT JOIN ("
            "  SELECT e2.thread_id, COUNT(*) AS thread_message_count "
            "  FROM emails e2 "
            "  WHERE e2.account_id = :account_id "
            "  GROUP BY e2.thread_id"
            ") AS thread_size ON thread_size.thread_id = e.thread_id "
            "LEFT JOIN ("
            "  SELECT e3.thread_id, MAX(e3.has_attachment) AS thread_has_attachment, "
            "         MAX(CASE WHEN seen3.email_id IS NULL THEN 1 ELSE 0 END) AS "
            "thread_has_unread, "
            "         MAX(CASE WHEN flagged3.email_id IS NULL THEN 0 ELSE 1 END) AS "
            "thread_has_flagged "
            "  FROM emails e3 "
            "  LEFT JOIN email_keywords seen3 ON seen3.account_id = e3.account_id "
            "       AND seen3.email_id = e3.email_id AND seen3.keyword = '$seen' "
            "  LEFT JOIN email_keywords flagged3 ON flagged3.account_id = e3.account_id "
            "       AND flagged3.email_id = e3.email_id AND flagged3.keyword = '$flagged' "
            "  WHERE e3.account_id = :account_id "
            "  GROUP BY e3.thread_id"
            ") AS thread_flags ON thread_flags.thread_id = e.thread_id "
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
                .threadMessageCount = query.value(6).toULongLong(),
                .hasAttachment = query.value(7).toInt() != 0,
                .isUnread = query.value(8).toInt() != 0,
                .isFlagged = query.value(9).toInt() != 0,
                .from = std::nullopt,
            };

            if (!query.value(11).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(10).isNull()
                                ? std::nullopt
                                : std::optional{query.value(10).toString().toStdString()},
                    .email = query.value(11).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

        return items;
    }

    std::variant<std::optional<SearchWindowPage>, DatabaseError>
    QueryService::loadSearchWindow(const std::string_view accountId,
                                   const std::string_view queryKey, const std::size_t offset,
                                   const std::size_t limit) const
    {
        SearchWindowRepository repository{m_connection};
        const auto windowResult = repository.find(accountId, queryKey, offset, limit);
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
            .total = (*window)->total,
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
            "       1 AS thread_message_count, e.has_attachment, "
            "       CASE WHEN seen.email_id IS NULL THEN 1 ELSE 0 END AS is_unread, "
            "       CASE WHEN flagged.email_id IS NULL THEN 0 ELSE 1 END AS is_flagged, "
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
            "FROM threads t "
            "INNER JOIN json_each(t.email_ids_json) thread_email ON 1 = 1 "
            "INNER JOIN emails e ON e.account_id = t.account_id "
            "     AND e.email_id = thread_email.value "
            "LEFT JOIN email_keywords seen ON seen.account_id = e.account_id "
            "     AND seen.email_id = e.email_id AND seen.keyword = '$seen' "
            "LEFT JOIN email_keywords flagged ON flagged.account_id = e.account_id "
            "     AND flagged.email_id = e.email_id AND flagged.keyword = '$flagged' "
            "WHERE t.account_id = :account_id AND t.thread_id = :thread_id "
            "ORDER BY CAST(thread_email.key AS INTEGER) ASC"));
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
                .threadMessageCount = query.value(6).toULongLong(),
                .hasAttachment = query.value(7).toInt() != 0,
                .isUnread = query.value(8).toInt() != 0,
                .isFlagged = query.value(9).toInt() != 0,
                .from = std::nullopt,
            };

            if (!query.value(11).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(10).isNull()
                                ? std::nullopt
                                : std::optional{query.value(10).toString().toStdString()},
                    .email = query.value(11).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

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

} // namespace javelin::jmap::cache
