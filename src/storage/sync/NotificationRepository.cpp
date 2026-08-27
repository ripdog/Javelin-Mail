#include "jmap/cache/NotificationRepository.h"

#include "jmap/cache/NotificationDispatchRepository.h"

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_set>

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

    std::optional<DatabaseError> NotificationRepository::synchronizeMailboxHorizons(
        const std::string_view accountId, const std::vector<std::string>& enabledMailboxIds,
        const std::optional<std::string_view> currentEmailState)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Synchronize notification horizons"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        const std::unordered_set<std::string> enabled{enabledMailboxIds.begin(),
                                                      enabledMailboxIds.end()};
        QSqlQuery existing{m_connection.database()};
        existing.prepare(QStringLiteral(
            "SELECT mailbox_id FROM mail_notification_horizons WHERE account_id=:account_id"));
        existing.bindValue(QStringLiteral(":account_id"),
                           QString::fromStdString(std::string{accountId}));
        if (!existing.exec())
            return queryError(QStringLiteral("Read notification horizons"), existing);

        std::vector<std::string> disabledMailboxIds;
        while (existing.next())
        {
            const auto mailboxId = existing.value(0).toString().toStdString();
            if (!enabled.contains(mailboxId))
                disabledMailboxIds.push_back(mailboxId);
        }
        existing.finish();

        QSqlQuery remove{m_connection.database()};
        remove.prepare(QStringLiteral(
            "DELETE FROM mail_notification_horizons WHERE account_id=:account_id AND "
            "mailbox_id=:mailbox_id"));
        for (const auto& mailboxId : disabledMailboxIds)
        {
            remove.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{accountId}));
            remove.bindValue(QStringLiteral(":mailbox_id"), QString::fromStdString(mailboxId));
            if (!remove.exec())
                return queryError(QStringLiteral("Remove notification horizon"), remove);
            remove.finish();
        }

        if (currentEmailState.has_value())
        {
            QSqlQuery insert{m_connection.database()};
            insert.prepare(
                QStringLiteral("INSERT OR IGNORE INTO "
                               "mail_notification_horizons(account_id,mailbox_id,email_state) "
                               "VALUES(:account_id,:mailbox_id,:email_state)"));
            for (const auto& mailboxId : enabledMailboxIds)
            {
                insert.bindValue(QStringLiteral(":account_id"),
                                 QString::fromStdString(std::string{accountId}));
                insert.bindValue(QStringLiteral(":mailbox_id"), QString::fromStdString(mailboxId));
                insert.bindValue(QStringLiteral(":email_state"),
                                 QString::fromStdString(std::string{*currentEmailState}));
                if (!insert.exec())
                    return queryError(QStringLiteral("Create notification horizon"), insert);
                insert.finish();
            }
        }
        return transaction.commit();
    }

    std::variant<std::vector<std::string>, DatabaseError>
    NotificationRepository::mailboxHorizonsAtState(const std::string_view accountId,
                                                   const std::string_view emailState) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT mailbox_id FROM mail_notification_horizons WHERE account_id=:account_id AND "
            "email_state=:email_state ORDER BY mailbox_id"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_state"),
                        QString::fromStdString(std::string{emailState}));
        if (!query.exec())
            return queryError(QStringLiteral("Read notification horizons at Email state"), query);
        std::vector<std::string> mailboxIds;
        while (query.next())
            mailboxIds.push_back(query.value(0).toString().toStdString());
        return mailboxIds;
    }

    std::variant<bool, DatabaseError>
    NotificationRepository::wasCreatedByMailImport(const std::string_view accountId,
                                                   const std::string_view emailId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT 1 FROM mail_import_items item JOIN mail_import_operations operation ON "
            "operation.operation_id=item.operation_id WHERE operation.account_id=:account_id AND "
            "item.created_email_id=:email_id LIMIT 1"));
        query.bindValue(QStringLiteral(":account_id"),
                        QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":email_id"), QString::fromStdString(std::string{emailId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read mail import notification provenance"), query);
        return query.next();
    }

    std::optional<DatabaseError> NotificationRepository::advanceMailboxHorizons(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view expectedEmailState, const std::string_view newEmailState)
    {
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral(
                    "Notification horizon advancement requires an active matching transaction."),
            };
        }
        QSqlQuery update{m_connection.database()};
        update.prepare(
            QStringLiteral("UPDATE mail_notification_horizons SET email_state=:new_state WHERE "
                           "account_id=:account_id AND email_state=:expected_state"));
        update.bindValue(QStringLiteral(":new_state"),
                         QString::fromStdString(std::string{newEmailState}));
        update.bindValue(QStringLiteral(":account_id"),
                         QString::fromStdString(std::string{accountId}));
        update.bindValue(QStringLiteral(":expected_state"),
                         QString::fromStdString(std::string{expectedEmailState}));
        if (!update.exec())
            return queryError(QStringLiteral("Advance notification horizons"), update);
        return std::nullopt;
    }

    std::variant<std::vector<MailNotificationPendingEvent>, DatabaseError>
    NotificationRepository::claimPendingEvents(const std::string_view accountId)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Claim pending mail notification events"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery pending{m_connection.database()};
        pending.prepare(QStringLiteral(
            "SELECT "
            "event.mailbox_id,event.email_id,event.thread_id,event.subject,event.received_at,"
            "EXISTS(SELECT 1 FROM email_mailboxes membership WHERE "
            "membership.account_id=event.account_id AND membership.email_id=event.email_id AND "
            "membership.mailbox_id=event.mailbox_id),"
            "EXISTS(SELECT 1 FROM email_keywords keyword WHERE keyword.account_id=event.account_id "
            "AND keyword.email_id=event.email_id AND keyword.keyword='$seen'),"
            "EXISTS(SELECT 1 FROM mail_notification_horizons horizon WHERE "
            "horizon.account_id=event.account_id AND horizon.mailbox_id=event.mailbox_id) "
            "FROM mail_notification_event_outbox event WHERE event.account_id=:account_id "
            "ORDER BY event.mailbox_id,event.created_at,event.email_id"));
        pending.bindValue(QStringLiteral(":account_id"),
                          QString::fromStdString(std::string{accountId}));
        if (!pending.exec())
            return queryError(QStringLiteral("Read pending mail notification events"), pending);

        struct PendingCandidate
        {
            MailNotificationPendingEvent event;
            bool eligible = false;
        };
        std::vector<PendingCandidate> candidates;
        while (pending.next())
        {
            candidates.push_back(PendingCandidate{
                .event =
                    {
                        .accountId = std::string{accountId},
                        .mailboxId = pending.value(0).toString().toStdString(),
                        .emailId = pending.value(1).toString().toStdString(),
                        .threadId = pending.value(2).toString().toStdString(),
                        .subject = pending.value(3).isNull()
                                       ? std::nullopt
                                       : std::optional{pending.value(3).toString().toStdString()},
                        .receivedAt = pending.value(4).toString().toStdString(),
                    },
                .eligible = pending.value(5).toBool() && !pending.value(6).toBool() &&
                            pending.value(7).toBool(),
            });
        }
        pending.finish();

        QSqlQuery cancel{m_connection.database()};
        cancel.prepare(QStringLiteral(
            "DELETE FROM mail_notification_event_outbox WHERE account_id=:account_id AND "
            "email_id=:email_id"));
        NotificationDispatchRepository dispatches{m_connection};
        std::vector<MailNotificationPendingEvent> events;
        for (auto& candidate : candidates)
        {
            if (!candidate.eligible)
            {
                cancel.bindValue(QStringLiteral(":account_id"),
                                 QString::fromStdString(std::string{accountId}));
                cancel.bindValue(QStringLiteral(":email_id"),
                                 QString::fromStdString(candidate.event.emailId));
                if (!cancel.exec())
                    return queryError(QStringLiteral("Cancel stale mail notification event"),
                                      cancel);
                cancel.finish();
                continue;
            }

            const auto claimed = dispatches.claim(transaction, NotificationDispatchKind::Mail,
                                                  mailClaimKey(accountId, candidate.event.emailId));
            if (const auto* error = std::get_if<DatabaseError>(&claimed))
                return *error;
            if (!std::get<bool>(claimed))
                continue;
            events.push_back(std::move(candidate.event));
        }
        if (const auto error = transaction.commit())
            return *error;
        return events;
    }

    std::optional<DatabaseError>
    NotificationRepository::markDelivered(const std::string_view accountId,
                                          const std::vector<std::string>& emailIds)
    {
        if (emailIds.empty())
            return std::nullopt;
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Complete mail notification delivery"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery remove{m_connection.database()};
        remove.prepare(QStringLiteral(
            "DELETE FROM mail_notification_event_outbox WHERE account_id=:account_id AND "
            "email_id=:email_id"));
        NotificationDispatchRepository dispatches{m_connection};
        for (const auto& emailId : emailIds)
        {
            remove.bindValue(QStringLiteral(":account_id"),
                             QString::fromStdString(std::string{accountId}));
            remove.bindValue(QStringLiteral(":email_id"), QString::fromStdString(emailId));
            if (!remove.exec())
                return queryError(QStringLiteral("Complete mail notification event"), remove);
            remove.finish();
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
