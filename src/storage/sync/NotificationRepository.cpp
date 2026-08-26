#include "jmap/cache/NotificationRepository.h"

#include "jmap/cache/NotificationDispatchRepository.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        [[nodiscard]] std::string mailClaimKey(const std::string_view accountId,
                                               const std::string_view emailId)
        {
            return std::to_string(accountId.size()) + ":" + std::string{accountId} +
                   std::string{emailId};
        }
    } // namespace

    NotificationRepository::NotificationRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<bool, DatabaseError>
    NotificationRepository::createEventIfUnconsumed(DatabaseTransaction& transaction,
                                                    const std::string_view accountId,
                                                    const MailNotificationEventInput& event)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Mail notification event creation requires an active matching transaction."),
            };
        }

        QSqlQuery consume{m_connection.database()};
        consume.prepare(
            QStringLiteral("INSERT OR IGNORE INTO mail_notification_state(account_id,email_id) "
                           "VALUES(:account_id,:email_id)"));
        consume.bindValue(QStringLiteral(":account_id"),
                          QString::fromStdString(std::string{accountId}));
        consume.bindValue(QStringLiteral(":email_id"), QString::fromStdString(event.emailId));
        if (!consume.exec())
            return queryError(QStringLiteral("Consume mail notification event"), consume);
        if (consume.numRowsAffected() == 0)
            return false;

        QSqlQuery enqueue{m_connection.database()};
        enqueue.prepare(
            QStringLiteral("INSERT INTO mail_notification_event_outbox "
                           "(account_id,email_id,mailbox_id,thread_id,subject,received_at) VALUES "
                           "(:account_id,:email_id,:mailbox_id,:thread_id,:subject,:received_at)"));
        enqueue.bindValue(QStringLiteral(":account_id"),
                          QString::fromStdString(std::string{accountId}));
        enqueue.bindValue(QStringLiteral(":email_id"), QString::fromStdString(event.emailId));
        enqueue.bindValue(QStringLiteral(":mailbox_id"), QString::fromStdString(event.mailboxId));
        enqueue.bindValue(QStringLiteral(":thread_id"), QString::fromStdString(event.threadId));
        if (event.subject.has_value())
            enqueue.bindValue(QStringLiteral(":subject"), QString::fromStdString(*event.subject));
        else
            enqueue.bindValue(QStringLiteral(":subject"), QVariant{});
        enqueue.bindValue(QStringLiteral(":received_at"), QString::fromStdString(event.receivedAt));
        if (!enqueue.exec())
            return queryError(QStringLiteral("Queue mail notification delivery"), enqueue);
        return true;
    }

    std::variant<std::vector<MailNotificationPendingEvent>, DatabaseError>
    NotificationRepository::listPendingEvents(const std::string_view accountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT mailbox_id,email_id,thread_id,subject,received_at FROM "
                           "mail_notification_event_outbox WHERE account_id=:account_id ORDER BY "
                           "created_at,email_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("List pending mail notification events"), query);

        std::vector<MailNotificationPendingEvent> events;
        while (query.next())
        {
            events.push_back(MailNotificationPendingEvent{
                .accountId = std::string{accountId},
                .mailboxId = query.value(0).toString().toStdString(),
                .emailId = query.value(1).toString().toStdString(),
                .threadId = query.value(2).toString().toStdString(),
                .subject = query.value(3).isNull()
                               ? std::nullopt
                               : std::optional{query.value(3).toString().toStdString()},
                .receivedAt = query.value(4).toString().toStdString(),
            });
        }
        return events;
    }

    std::variant<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>, DatabaseError>
    NotificationRepository::enqueueUnreadMailboxEmails(const std::string_view accountId,
                                                       const std::string_view mailboxId)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Claim pending mail notifications"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        auto& database = m_connection.database();

        QSqlQuery emails{database};
        emails.prepare(QStringLiteral(
            "WITH visible_threads AS MATERIALIZED ("
            "SELECT DISTINCT representative.thread_id FROM mailbox_query_windows w "
            "JOIN mailbox_query_window_items i ON i.account_id=w.account_id "
            "AND i.query_key=w.query_key AND i.requested_offset=w.requested_offset "
            "AND i.requested_limit=w.requested_limit "
            "JOIN emails representative ON representative.account_id=i.account_id "
            "AND representative.email_id=i.email_id "
            "WHERE w.account_id=:account_id AND w.mailbox_id=:mailbox_id "
            "AND w.coverage='server'"
            ") SELECT e.email_id, e.thread_id, e.subject, e.received_at, "
            "EXISTS(SELECT 1 FROM email_keywords k WHERE k.account_id = e.account_id "
            "AND k.email_id = e.email_id AND k.keyword = '$seen') "
            "FROM visible_threads v CROSS JOIN emails e INDEXED BY idx_emails_thread "
            "ON e.account_id=:account_id AND e.thread_id=v.thread_id "
            "JOIN email_mailboxes m ON m.account_id=e.account_id AND m.email_id=e.email_id "
            "AND m.mailbox_id=:mailbox_id "
            "WHERE NOT EXISTS(SELECT 1 FROM observed_notification_emails o "
            "WHERE o.account_id=e.account_id AND o.email_id=e.email_id) "
            "ORDER BY e.received_at,e.email_id"));
        emails.bindValue(QStringLiteral(":account_id"),
                         QString::fromStdString(std::string{accountId}));
        emails.bindValue(QStringLiteral(":mailbox_id"),
                         QString::fromStdString(std::string{mailboxId}));
        if (!emails.exec())
            return queryError(QStringLiteral("Read notification mailbox"), emails);

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
                return queryError(QStringLiteral("Record observed notification email"), observe);
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
                    return queryError(QStringLiteral("Enqueue mail notification"), enqueue);
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
            return queryError(QStringLiteral("Read pending mail notifications"), pending);

        NotificationDispatchRepository dispatches{m_connection};
        std::vector<javelin::jmap::sync::RefreshNotificationCandidate> candidates;
        while (pending.next())
        {
            const auto emailId = pending.value(0).toString().toStdString();
            const auto claimed = dispatches.claim(transaction, NotificationDispatchKind::Mail,
                                                  mailClaimKey(accountId, emailId));
            if (const auto* error = std::get_if<DatabaseError>(&claimed))
                return *error;
            if (!std::get<bool>(claimed))
                continue;
            candidates.push_back(javelin::jmap::sync::RefreshNotificationCandidate{
                .emailId = emailId,
                .threadId = pending.value(1).toString().toStdString(),
                .subject = pending.value(2).isNull()
                               ? std::nullopt
                               : std::optional{pending.value(2).toString().toStdString()},
                .receivedAt = pending.value(3).toString().toStdString(),
            });
        }
        if (const auto error = transaction.commit())
            return *error;
        return candidates;
    }

    std::optional<DatabaseError>
    NotificationRepository::markDelivered(const std::string_view accountId,
                                          const std::string_view mailboxId,
                                          const std::vector<std::string>& emailIds)
    {
        if (emailIds.empty())
            return std::nullopt;
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Complete mail notification delivery"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery update{m_connection.database()};
        update.prepare(QStringLiteral(
            "UPDATE mail_notification_outbox SET status='delivered',delivered_at=CURRENT_TIMESTAMP "
            "WHERE account_id=:account_id AND mailbox_id=:mailbox_id AND email_id=:email_id "
            "AND status='pending'"));
        NotificationDispatchRepository dispatches{m_connection};
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
            if (const auto error = dispatches.release(transaction, NotificationDispatchKind::Mail,
                                                      mailClaimKey(accountId, emailId)))
                return error;
        }
        return transaction.commit();
    }

    std::optional<DatabaseError>
    NotificationRepository::releaseDispatches(const std::string_view accountId,
                                              const std::vector<std::string>& emailIds)
    {
        if (emailIds.empty())
            return std::nullopt;
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Release mail notification delivery"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        NotificationDispatchRepository dispatches{m_connection};
        for (const auto& emailId : emailIds)
            if (const auto error = dispatches.release(transaction, NotificationDispatchKind::Mail,
                                                      mailClaimKey(accountId, emailId)))
                return error;
        return transaction.commit();
    }

    std::optional<DatabaseError> NotificationRepository::recoverDispatches()
    {
        NotificationDispatchRepository dispatches{m_connection};
        return dispatches.recover(NotificationDispatchKind::Mail);
    }

} // namespace javelin::jmap::cache
