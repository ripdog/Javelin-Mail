#include "jmap/cache/MailboxStatisticsReadRepository.h"

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
    } // namespace

    MailboxStatisticsReadRepository::MailboxStatisticsReadRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxStatisticsReadRepository::MailboxStatisticsReadRepository(
        ReadOnlyDatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    MailboxStatisticsReadRepository::MailboxStatisticsReadRepository(DatabaseReadView connection)
        : m_connection(connection)
    {
    }

    std::variant<std::size_t, DatabaseError>
    MailboxStatisticsReadRepository::countMailboxMessages(const std::string_view accountId,
                                                          const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

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
            return makeQueryError(QStringLiteral("Count mailbox message list"), query);
        if (!query.next())
            return static_cast<std::size_t>(0);
        return static_cast<std::size_t>(query.value(0).toULongLong());
    }

    std::variant<std::size_t, DatabaseError>
    MailboxStatisticsReadRepository::countUnreadMailboxEmails(
        const std::string_view accountId, const std::string_view mailboxId) const
    {
        if (const auto error = m_connection.validate())
            return *error;

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
            return makeQueryError(QStringLiteral("Count unread mailbox emails"), query);
        if (!query.next())
            return static_cast<std::size_t>(0);
        return static_cast<std::size_t>(query.value(0).toULongLong());
    }

} // namespace javelin::jmap::cache
