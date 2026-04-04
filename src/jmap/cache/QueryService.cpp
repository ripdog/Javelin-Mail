#include "jmap/cache/QueryService.h"

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
                .message = operation + ": " + query.lastError().text(),
            };
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
        query.prepare(
            "SELECT m.mailbox_id, m.name, m.parent_mailbox_id, m.role, m.sort_order, "
            "m.total_emails, m.unread_emails, m.total_threads, m.unread_threads, "
            "m.is_subscribed, "
            "EXISTS("
            "  SELECT 1 FROM mailboxes child "
            "  WHERE child.account_id = m.account_id AND child.parent_mailbox_id = m.mailbox_id"
            ") AS has_children "
            "FROM mailboxes m "
            "WHERE m.account_id = :account_id "
            "ORDER BY COALESCE(m.parent_mailbox_id, ''), m.sort_order, m.mailbox_id");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        if (!query.exec())
        {
            return makeQueryError("Read mailbox tree", query);
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

    std::variant<std::vector<MessageListItem>, DatabaseError>
    QueryService::listMailboxMessages(const std::string_view accountId,
                                      const std::string_view mailboxId, const std::size_t limit,
                                      const std::size_t offset) const
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        QSqlQuery query{m_connection.database()};
        query.prepare(
            "SELECT e.email_id, e.thread_id, e.subject, e.preview, e.received_at, e.sent_at, "
            "e.has_attachment, "
            "EXISTS("
            "  SELECT 1 FROM email_keywords k "
            "  WHERE k.account_id = e.account_id AND k.email_id = e.email_id AND k.keyword = "
            "'$seen'"
            ") AS has_seen, "
            "EXISTS("
            "  SELECT 1 FROM email_keywords k "
            "  WHERE k.account_id = e.account_id AND k.email_id = e.email_id AND k.keyword = "
            "'$flagged'"
            ") AS has_flagged, "
            "("
            "  SELECT a.display_name FROM email_addresses a "
            "  WHERE a.account_id = e.account_id AND a.email_id = e.email_id AND a.field_name = "
            "'from' "
            "  ORDER BY a.position LIMIT 1"
            ") AS from_name, "
            "("
            "  SELECT a.address FROM email_addresses a "
            "  WHERE a.account_id = e.account_id AND a.email_id = e.email_id AND a.field_name = "
            "'from' "
            "  ORDER BY a.position LIMIT 1"
            ") AS from_email "
            "FROM emails e "
            "INNER JOIN email_mailboxes em ON em.account_id = e.account_id AND em.email_id = "
            "e.email_id "
            "WHERE e.account_id = :account_id AND em.mailbox_id = :mailbox_id "
            "ORDER BY e.received_at DESC, e.email_id DESC "
            "LIMIT :limit OFFSET :offset");
        query.bindValue(":account_id", QString::fromStdString(std::string{accountId}));
        query.bindValue(":mailbox_id", QString::fromStdString(std::string{mailboxId}));
        query.bindValue(":limit", static_cast<qulonglong>(limit));
        query.bindValue(":offset", static_cast<qulonglong>(offset));
        if (!query.exec())
        {
            return makeQueryError("Read mailbox message list", query);
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
                .hasAttachment = query.value(6).toInt() != 0,
                .isUnread = query.value(7).toInt() == 0,
                .isFlagged = query.value(8).toInt() != 0,
                .from = std::nullopt,
            };

            if (!query.value(10).isNull())
            {
                item.from = javelin::jmap::domain::EmailAddress{
                    .name = query.value(9).isNull()
                                ? std::nullopt
                                : std::optional{query.value(9).toString().toStdString()},
                    .email = query.value(10).toString().toStdString(),
                };
            }

            items.push_back(std::move(item));
        }

        return items;
    }

} // namespace javelin::jmap::cache
