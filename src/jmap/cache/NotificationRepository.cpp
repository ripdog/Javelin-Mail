#include "jmap/cache/NotificationRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = operation + QStringLiteral(": ") + query.lastError().text(),
            };
        }
    } // namespace

    NotificationRepository::NotificationRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>, DatabaseError>
    NotificationRepository::enqueueUnreadMailboxEmails(const std::string_view accountId,
                                                       const std::string_view mailboxId)
    {
        if (const auto error = m_connection.validate())
        {
            return *error;
        }

        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin notification claim: ") +
                                            database.lastError().text()};
        }

        QSqlQuery emails{database};
        emails.prepare(QStringLiteral(
            "SELECT e.email_id, e.thread_id, e.subject, e.received_at, "
            "EXISTS(SELECT 1 FROM email_keywords k WHERE k.account_id = e.account_id "
            "AND k.email_id = e.email_id AND k.keyword = '$seen') "
            "FROM emails e JOIN email_mailboxes m ON m.account_id = e.account_id "
            "AND m.email_id = e.email_id WHERE e.account_id = :account_id "
            "AND m.mailbox_id = :mailbox_id ORDER BY e.received_at, e.email_id"));
        emails.bindValue(QStringLiteral(":account_id"),
                         QString::fromStdString(std::string{accountId}));
        emails.bindValue(QStringLiteral(":mailbox_id"),
                         QString::fromStdString(std::string{mailboxId}));
        if (!emails.exec())
        {
            database.rollback();
            return queryError(QStringLiteral("Read notification mailbox"), emails);
        }

        QSqlQuery observe{database};
        observe.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO observed_notification_emails (account_id, email_id) "
            "VALUES (:account_id, :email_id)"));

        QSqlQuery enqueue{database};
        enqueue.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO mail_notification_outbox "
            "(account_id,mailbox_id,email_id,thread_id,subject,received_at,status) VALUES "
            "(:account_id,:mailbox_id,:email_id,:thread_id,:subject,:received_at,'pending')"));

        while (emails.next())
        {
            const auto emailId = emails.value(0).toString();
            observe.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(std::string{accountId}));
            observe.bindValue(QStringLiteral(":email_id"), emailId);
            if (!observe.exec())
            {
                database.rollback();
                return queryError(QStringLiteral("Record observed notification email"), observe);
            }
            if (observe.numRowsAffected() == 1 && !emails.value(4).toBool())
            {
                enqueue.bindValue(QStringLiteral(":account_id"),
                                  QString::fromStdString(std::string{accountId}));
                enqueue.bindValue(QStringLiteral(":mailbox_id"),
                                  QString::fromStdString(std::string{mailboxId}));
                enqueue.bindValue(QStringLiteral(":email_id"), emailId);
                enqueue.bindValue(QStringLiteral(":thread_id"), emails.value(1));
                enqueue.bindValue(QStringLiteral(":subject"), emails.value(2));
                enqueue.bindValue(QStringLiteral(":received_at"), emails.value(3));
                if (!enqueue.exec())
                {
                    database.rollback();
                    return queryError(QStringLiteral("Enqueue mail notification"), enqueue);
                }
                enqueue.finish();
            }
            observe.finish();
        }

        QSqlQuery pending{database};
        pending.prepare(QStringLiteral(
            "SELECT email_id,thread_id,subject,received_at FROM mail_notification_outbox "
            "WHERE account_id=:account_id AND mailbox_id=:mailbox_id AND status='pending' "
            "ORDER BY received_at,email_id"));
        pending.bindValue(QStringLiteral(":account_id"),
                          QString::fromStdString(std::string{accountId}));
        pending.bindValue(QStringLiteral(":mailbox_id"),
                          QString::fromStdString(std::string{mailboxId}));
        if (!pending.exec())
        {
            database.rollback();
            return queryError(QStringLiteral("Read pending mail notifications"), pending);
        }

        std::vector<javelin::jmap::sync::RefreshNotificationCandidate> candidates;
        while (pending.next())
        {
            candidates.push_back(javelin::jmap::sync::RefreshNotificationCandidate{
                .emailId = pending.value(0).toString().toStdString(),
                .threadId = pending.value(1).toString().toStdString(),
                .subject = pending.value(2).isNull()
                               ? std::nullopt
                               : std::optional{pending.value(2).toString().toStdString()},
                .receivedAt = pending.value(3).toString().toStdString(),
            });
        }

        if (!database.commit())
        {
            database.rollback();
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit notification claim: ") +
                                            database.lastError().text()};
        }
        return candidates;
    }

    std::optional<DatabaseError>
    NotificationRepository::markDelivered(const std::string_view accountId,
                                          const std::string_view mailboxId,
                                          const std::vector<std::string>& emailIds)
    {
        if (emailIds.empty())
            return std::nullopt;
        if (const auto error = m_connection.validate())
            return error;

        QSqlQuery update{m_connection.database()};
        update.prepare(QStringLiteral(
            "UPDATE mail_notification_outbox SET status='delivered',delivered_at=CURRENT_TIMESTAMP "
            "WHERE account_id=:account_id AND mailbox_id=:mailbox_id AND email_id=:email_id "
            "AND status='pending'"));
        for (const auto& emailId : emailIds)
        {
            update.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{accountId}));
            update.bindValue(QStringLiteral(":mailbox_id"),
                             QString::fromStdString(std::string{mailboxId}));
            update.bindValue(QStringLiteral(":email_id"), QString::fromStdString(emailId));
            if (!update.exec())
                return queryError(QStringLiteral("Mark mail notification delivered"), update);
            update.finish();
        }
        return std::nullopt;
    }

} // namespace javelin::jmap::cache
