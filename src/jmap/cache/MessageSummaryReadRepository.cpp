#include "jmap/cache/MessageSummaryReadRepository.h"

#include "jmap/cache/MessageListReadSupport.h"

#include <glaze/glaze.hpp>

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
    } // namespace

    MessageSummaryReadRepository::MessageSummaryReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MessageSummaryReadRepository::MessageSummaryReadRepository(
        ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MessageSummaryReadRepository::MessageSummaryReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    MessageSummaryReadRepository::listMessagesByEmailIds(
        const std::string_view accountId, const std::vector<std::string>& emailIds) const
    {
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
            return makeQueryError(QStringLiteral("Read message list by email ids"), query);

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

        if (const auto error = detail::attachMessageListMetadata(m_connection, accountId, items))
            return *error;
        return items;
    }

    std::variant<std::optional<MessageListItem>, DatabaseError>
    MessageSummaryReadRepository::findMailboxMessage(const std::string_view accountId,
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
            "           AND NOT EXISTS(SELECT 1 FROM email_summary_refresh_requests refresh "
            "             WHERE refresh.account_id=thread_member.account_id "
            "               AND refresh.email_id=thread_member.email_id) "
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
        if (const auto error = detail::attachMessageListMetadata(m_connection, accountId, items))
            return *error;
        return std::optional<MessageListItem>{std::move(items.front())};
    }

} // namespace javelin::jmap::cache
