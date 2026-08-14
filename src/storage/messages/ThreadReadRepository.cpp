#include "jmap/cache/ThreadReadRepository.h"

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

        [[nodiscard]] MessageListItem messageListItemFromQuery(const QSqlQuery& query)
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
            return item;
        }

        [[nodiscard]] std::variant<std::vector<MessageListItem>, DatabaseError>
        collectThreadItems(QSqlQuery& query, const DatabaseReadView& connection,
                           const std::string_view accountId, const QString& operation)
        {
            if (!query.exec())
                return makeQueryError(operation, query);

            std::vector<MessageListItem> items;
            while (query.next())
                items.push_back(messageListItemFromQuery(query));

            if (const auto error = detail::attachMessageListMetadata(connection, accountId, items))
                return *error;
            return items;
        }
    } // namespace

    ThreadReadRepository::ThreadReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    ThreadReadRepository::ThreadReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    ThreadReadRepository::ThreadReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    ThreadReadRepository::listThreadMessages(const std::string_view accountId,
                                             const std::string_view threadId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

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
        return collectThreadItems(query, m_connection, accountId,
                                  QStringLiteral("Read thread messages"));
    }

    std::variant<std::vector<MessageListItem>, DatabaseError>
    ThreadReadRepository::listMailboxThreadMessages(
        const std::string_view accountId, const std::string_view mailboxId,
        const std::string_view threadId, const MailboxThreadMembershipSource membershipSource) const
    {
        if (const auto error = m_connection.validate())
            return *error;

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
            membershipSource == MailboxThreadMembershipSource::CachedMailbox
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
            membershipSource == MailboxThreadMembershipSource::CachedMailbox
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
        return collectThreadItems(query, m_connection, accountId,
                                  QStringLiteral("Read mailbox thread messages"));
    }

} // namespace javelin::jmap::cache
