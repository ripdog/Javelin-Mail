#include "jmap/cache/MailboxMessageReadRepository.h"

#include "jmap/cache/EmailListSortSql.h"
#include "jmap/cache/MessageListReadSupport.h"

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

        [[nodiscard]] std::optional<std::uint64_t> optionalCount(const QVariant& value)
        {
            return value.isNull() ? std::nullopt
                                  : std::optional<std::uint64_t>{value.toULongLong()};
        }

        [[nodiscard]] MessageListItem messageListItemFromMailboxRow(const QSqlQuery& query)
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
            return item;
        }
    } // namespace

    MailboxMessageReadRepository::MailboxMessageReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxMessageReadRepository::MailboxMessageReadRepository(
        ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxMessageReadRepository::MailboxMessageReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    MailboxMessageReadRepository::listMailboxMessages(
        const std::string_view accountId, const std::string_view mailboxId, const std::size_t limit,
        const std::size_t offset, javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = detail::emailListSortKeyExpression(sort.property);
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
                "                            ORDER BY %2 %1, e.email_id %1) AS thread_rank, "
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
                "            AND NOT EXISTS(SELECT 1 FROM email_summary_refresh_requests refresh "
                "              WHERE refresh.account_id=member.account_id "
                "                AND refresh.email_id=member.email_id) "
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
                "= 'from' "
                "  ORDER BY a.position LIMIT 1"
                ") AS from_name, "
                "("
                "  SELECT a.address FROM email_addresses a "
                "  WHERE a.account_id = :account_id AND a.email_id = rt.email_id AND a.field_name "
                "= 'from' "
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
            return makeQueryError(QStringLiteral("Read mailbox message list"), query);

        std::vector<MessageListItem> items;
        while (query.next())
            items.push_back(messageListItemFromMailboxRow(query));

        if (const auto error = detail::attachMessageListMetadata(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::optional<OfflineMailboxCoverage>, DatabaseError>
    MailboxMessageReadRepository::offlineMailboxCoverage(const std::string_view accountId,
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
    MailboxMessageReadRepository::offlineMailboxComplete(const std::string_view accountId,
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
    MailboxMessageReadRepository::completeOfflineMailboxQueryState(
        const std::string_view accountId, const std::string_view mailboxId,
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
    MailboxMessageReadRepository::listOfflineMailboxRepresentativeIds(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::uint64_t generation, const std::size_t limit, const std::size_t offset,
        javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = detail::emailListSortKeyExpression(sort.property);
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
    MailboxMessageReadRepository::listOfflineMailboxMessages(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::uint64_t generation, const std::size_t limit, const std::size_t offset,
        javelin::jmap::query::EmailListSort sort) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        const auto orderDirection = javelin::jmap::query::isAscending(sort)
                                        ? QStringLiteral("ASC")
                                        : QStringLiteral("DESC");
        const auto sortKey = detail::emailListSortKeyExpression(sort.property);
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
                "            AND NOT EXISTS(SELECT 1 FROM email_summary_refresh_requests refresh "
                "              WHERE refresh.account_id=member.account_id "
                "                AND refresh.email_id=member.email_id) "
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
            items.push_back(messageListItemFromMailboxRow(query));

        if (const auto error = detail::attachMessageListMetadata(m_connection, accountId, items))
            return *error;
        return items;
    }

} // namespace javelin::jmap::cache
