#include "jmap/cache/MailboxReadRepository.h"

#include "jmap/cache/MailboxRepository.h"

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

    } // namespace

    MailboxReadRepository::MailboxReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxReadRepository::MailboxReadRepository(ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<MailboxTreeItem>, DatabaseError>
    MailboxReadRepository::listMailboxTree(const std::string_view accountId) const
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
                .pendingCreate = false,
            });
        }

        QSqlQuery pending{m_connection.database()};
        pending.prepare(QStringLiteral(
            "SELECT p.creation_id,p.name,p.parent_mailbox_id,p.sort_order,p.is_subscribed FROM "
            "mailbox_create_projections p WHERE p.account_id=:account_id AND NOT EXISTS(SELECT 1 "
            "FROM mailboxes m WHERE m.account_id=p.account_id AND m.name=p.name AND "
            "((m.parent_mailbox_id IS NULL AND p.parent_mailbox_id IS NULL) OR "
            "m.parent_mailbox_id=p.parent_mailbox_id)) ORDER BY p.sort_order,p.name"));
        pending.bindValue(QStringLiteral(":account_id"),
                          QString::fromStdString(std::string{accountId}));
        if (!pending.exec())
            return makeQueryError(QStringLiteral("Read pending mailbox creations"), pending);
        while (pending.next())
        {
            items.push_back(MailboxTreeItem{
                .id = QStringLiteral("pending-mailbox:%1")
                          .arg(pending.value(0).toString())
                          .toStdString(),
                .name = pending.value(1).toString().toStdString(),
                .parentId = pending.value(2).isNull()
                                ? std::nullopt
                                : std::optional{pending.value(2).toString().toStdString()},
                .role = std::nullopt,
                .sortOrder = pending.value(3).toULongLong(),
                .totalEmails = 0,
                .unreadEmails = 0,
                .totalThreads = 0,
                .unreadThreads = 0,
                .isSubscribed = pending.value(4).toInt() != 0,
                .myRights = {},
                .hasChildren = false,
                .pendingCreate = true,
            });
        }

        return items;
    }

} // namespace javelin::jmap::cache
