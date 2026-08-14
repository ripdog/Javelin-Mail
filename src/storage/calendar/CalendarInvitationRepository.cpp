#include "jmap/cache/CalendarInvitationRepository.h"

#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/cache/NotificationDispatchRepository.h"

#include <QCryptographicHash>
#include <QDate>
#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return databaseError(operation, query.lastError());
        }

        [[nodiscard]] QString typeValue(const calendar::CalendarEventNotificationType type)
        {
            switch (type)
            {
            case calendar::CalendarEventNotificationType::Created:
                return QStringLiteral("created");
            case calendar::CalendarEventNotificationType::Updated:
                return QStringLiteral("updated");
            case calendar::CalendarEventNotificationType::Destroyed:
                return QStringLiteral("destroyed");
            }
            Q_UNREACHABLE();
        }

        [[nodiscard]] QVariant optionalVariant(const std::optional<std::string>& value)
        {
            return value ? QVariant{QString::fromStdString(*value)} : QVariant{};
        }

        [[nodiscard]] std::string invitationKey(const std::string_view accountId,
                                                const std::string_view eventId)
        {
            QByteArray input{accountId.data(), static_cast<qsizetype>(accountId.size())};
            input.push_back('\0');
            input.append(eventId.data(), static_cast<qsizetype>(eventId.size()));
            return QCryptographicHash::hash(input, QCryptographicHash::Sha256)
                .toHex()
                .toStdString();
        }

        [[nodiscard]] std::string organizerName(const calendar::CalendarEvent& event)
        {
            if (event.organizerCalendarAddress)
            {
                const auto found = std::ranges::find_if(
                    event.attendees,
                    [&event](const calendar::Attendee& attendee)
                    {
                        return attendee.calendarAddress == *event.organizerCalendarAddress ||
                               attendee.isOwner;
                    });
                if (found != event.attendees.end() && !found->name.empty())
                    return found->name;
                return *event.organizerCalendarAddress;
            }
            const auto owner =
                std::ranges::find(event.attendees, true, &calendar::Attendee::isOwner);
            return owner == event.attendees.end() ? std::string{} : owner->name;
        }
    } // namespace

    CalendarInvitationRepository::CalendarInvitationRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError> CalendarInvitationRepository::replaceParticipantIdentities(
        const std::string_view accountId,
        const std::vector<calendar::ParticipantIdentity>& identities)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Replace calendar participant identities"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery remove{m_connection.database()};
        remove.prepare(QStringLiteral(
            "DELETE FROM calendar_participant_identities WHERE account_id=:account"));
        remove.bindValue(QStringLiteral(":account"),
                         QString::fromStdString(std::string{accountId}));
        if (!remove.exec())
        {
            transaction.rollback();
            return queryError(QStringLiteral("Replace calendar participant identities"), remove);
        }

        QSqlQuery insert{m_connection.database()};
        insert.prepare(QStringLiteral(
            "INSERT INTO calendar_participant_identities (account_id,identity_id,name,"
            "calendar_address,is_default) VALUES (:account,:id,:name,:address,:default)"));
        for (const auto& identity : identities)
        {
            insert.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            insert.bindValue(QStringLiteral(":id"), QString::fromStdString(identity.id));
            insert.bindValue(QStringLiteral(":name"), QString::fromStdString(identity.name));
            insert.bindValue(QStringLiteral(":address"),
                             QString::fromStdString(identity.calendarAddress));
            insert.bindValue(QStringLiteral(":default"), identity.isDefault ? 1 : 0);
            if (!insert.exec())
            {
                transaction.rollback();
                return queryError(QStringLiteral("Store calendar participant identity"), insert);
            }
        }
        return transaction.commit();
    }

    std::optional<DatabaseError>
    CalendarInvitationRepository::reconcile(const CalendarInvitationReconciliation& reconciliation)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Reconcile calendar invitations"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        CalendarRepository calendars{m_connection};
        if (const auto error = calendars.projectEvents(
                transaction, reconciliation.accountId, reconciliation.eventState,
                reconciliation.events, reconciliation.nonRecurringOccurrences,
                reconciliation.destroyedEventIds))
            return error;

        auto& database = m_connection.database();
        if (reconciliation.replaceNotifications)
        {
            QSqlQuery replaceNotifications{database};
            replaceNotifications.prepare(QStringLiteral(
                "UPDATE calendar_event_notifications SET is_deleted=1 WHERE account_id=:account"));
            replaceNotifications.bindValue(QStringLiteral(":account"),
                                           QString::fromStdString(reconciliation.accountId));
            if (!replaceNotifications.exec())
            {
                transaction.rollback();
                return queryError(QStringLiteral("Replace calendar event notifications"),
                                  replaceNotifications);
            }
        }
        QSqlQuery upsertNotification{database};
        upsertNotification.prepare(QStringLiteral(
            "INSERT INTO calendar_event_notifications (account_id,notification_id,created,"
            "changed_by_name,changed_by_email,changed_by_principal_id,"
            "changed_by_calendar_address,comment,type,calendar_event_id,is_draft,"
            "event_document_json,event_patch_json,is_deleted) VALUES (:account,:id,:created,"
            ":changed_name,:changed_email,:changed_principal,:changed_address,:comment,:type,"
            ":event,:draft,:document,:patch,0) ON CONFLICT(account_id,notification_id) DO UPDATE "
            "SET created=excluded.created,changed_by_name=excluded.changed_by_name,"
            "changed_by_email=excluded.changed_by_email,changed_by_principal_id="
            "excluded.changed_by_principal_id,changed_by_calendar_address="
            "excluded.changed_by_calendar_address,comment=excluded.comment,type=excluded.type,"
            "calendar_event_id=excluded.calendar_event_id,is_draft=excluded.is_draft,"
            "event_document_json=excluded.event_document_json,event_patch_json="
            "excluded.event_patch_json,is_deleted=0"));
        for (const auto& notification : reconciliation.notifications)
        {
            const auto document = api::serializeCalendarEventDocument(notification.event);
            if (!document)
            {
                transaction.rollback();
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Serialize calendar event notification snapshot")};
            }
            upsertNotification.bindValue(QStringLiteral(":account"),
                                         QString::fromStdString(reconciliation.accountId));
            upsertNotification.bindValue(QStringLiteral(":id"),
                                         QString::fromStdString(notification.id));
            upsertNotification.bindValue(QStringLiteral(":created"),
                                         QString::fromStdString(notification.created.value));
            upsertNotification.bindValue(QStringLiteral(":changed_name"),
                                         QString::fromStdString(notification.changedBy.name));
            upsertNotification.bindValue(QStringLiteral(":changed_email"),
                                         optionalVariant(notification.changedBy.email));
            upsertNotification.bindValue(QStringLiteral(":changed_principal"),
                                         optionalVariant(notification.changedBy.principalId));
            upsertNotification.bindValue(QStringLiteral(":changed_address"),
                                         optionalVariant(notification.changedBy.calendarAddress));
            upsertNotification.bindValue(QStringLiteral(":comment"),
                                         optionalVariant(notification.comment));
            upsertNotification.bindValue(QStringLiteral(":type"), typeValue(notification.type));
            upsertNotification.bindValue(QStringLiteral(":event"),
                                         QString::fromStdString(notification.calendarEventId));
            upsertNotification.bindValue(
                QStringLiteral(":draft"),
                notification.isDraft ? QVariant{*notification.isDraft ? 1 : 0} : QVariant{});
            upsertNotification.bindValue(QStringLiteral(":document"),
                                         QString::fromStdString(*document));
            upsertNotification.bindValue(QStringLiteral(":patch"),
                                         optionalVariant(notification.eventPatchJson));
            if (!upsertNotification.exec())
            {
                transaction.rollback();
                return queryError(QStringLiteral("Store calendar event notification"),
                                  upsertNotification);
            }
        }

        QSqlQuery tombstoneNotification{database};
        tombstoneNotification.prepare(QStringLiteral(
            "INSERT INTO calendar_event_notifications(account_id,notification_id,is_deleted) "
            "VALUES(:account,:id,1) ON CONFLICT(account_id,notification_id) DO UPDATE SET "
            "is_deleted=1"));
        for (const auto& notificationId : reconciliation.deletedNotificationIds)
        {
            tombstoneNotification.bindValue(QStringLiteral(":account"),
                                            QString::fromStdString(reconciliation.accountId));
            tombstoneNotification.bindValue(QStringLiteral(":id"),
                                            QString::fromStdString(notificationId));
            if (!tombstoneNotification.exec())
            {
                transaction.rollback();
                return queryError(QStringLiteral("Tombstone calendar event notification"),
                                  tombstoneNotification);
            }
        }

        QSqlQuery storeState{database};
        storeState.prepare(QStringLiteral(
            "INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
            "(:account,'CalendarEventNotification',:state) ON CONFLICT(account_id,data_type) DO "
            "UPDATE SET state=excluded.state"));
        storeState.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(reconciliation.accountId));
        storeState.bindValue(QStringLiteral(":state"),
                             QString::fromStdString(reconciliation.notificationState));
        if (!storeState.exec())
        {
            transaction.rollback();
            return queryError(QStringLiteral("Store calendar notification state"), storeState);
        }

        std::unordered_map<std::string, const CalendarInvitationProjection*> pendingByEvent;
        pendingByEvent.reserve(reconciliation.pendingInvitations.size());
        for (const auto& pending : reconciliation.pendingInvitations)
            pendingByEvent.insert_or_assign(pending.eventId, &pending);

        QSqlQuery upsertPending{database};
        upsertPending.prepare(QStringLiteral(
            "INSERT INTO calendar_pending_invitations(account_id,event_id,self_participant_id,"
            "source_notification_id) VALUES(:account,:event,:participant,:source) ON "
            "CONFLICT(account_id,event_id) DO UPDATE SET self_participant_id="
            "excluded.self_participant_id,source_notification_id=COALESCE("
            "excluded.source_notification_id,calendar_pending_invitations.source_notification_id),"
            "last_seen_at=CURRENT_TIMESTAMP"));
        QSqlQuery ensureOutbox{database};
        ensureOutbox.prepare(QStringLiteral(
            "INSERT INTO calendar_invitation_outbox(invitation_key,account_id,event_id,"
            "self_participant_id,source_notification_id,status) VALUES(:key,:account,:event,"
            ":participant,:source,:status) ON CONFLICT(account_id,event_id) DO UPDATE SET "
            "self_participant_id=excluded.self_participant_id,source_notification_id=COALESCE("
            "excluded.source_notification_id,calendar_invitation_outbox.source_notification_id)"));
        QSqlQuery removePending{database};
        removePending.prepare(
            QStringLiteral("DELETE FROM calendar_pending_invitations WHERE account_id=:account AND "
                           "event_id=:event"));
        QSqlQuery resolveOutbox{database};
        resolveOutbox.prepare(QStringLiteral(
            "UPDATE calendar_invitation_outbox SET status='resolved',resolved_at="
            "COALESCE(resolved_at,CURRENT_TIMESTAMP) WHERE account_id=:account AND event_id=:event "
            "AND status<>'resolved'"));
        NotificationDispatchRepository dispatches{m_connection};

        std::unordered_set<std::string> considered{reconciliation.consideredEventIds.begin(),
                                                   reconciliation.consideredEventIds.end()};
        considered.insert(reconciliation.destroyedEventIds.begin(),
                          reconciliation.destroyedEventIds.end());
        for (const auto& eventId : considered)
        {
            const auto pending = pendingByEvent.find(eventId);
            if (pending != pendingByEvent.end())
            {
                const auto& projection = *pending->second;
                upsertPending.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(reconciliation.accountId));
                upsertPending.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
                upsertPending.bindValue(QStringLiteral(":participant"),
                                        QString::fromStdString(projection.selfParticipantId));
                upsertPending.bindValue(QStringLiteral(":source"),
                                        optionalVariant(projection.sourceNotificationId));
                if (!upsertPending.exec())
                {
                    transaction.rollback();
                    return queryError(QStringLiteral("Project pending calendar invitation"),
                                      upsertPending);
                }
                ensureOutbox.bindValue(
                    QStringLiteral(":key"),
                    QString::fromStdString(invitationKey(reconciliation.accountId, eventId)));
                ensureOutbox.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(reconciliation.accountId));
                ensureOutbox.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
                ensureOutbox.bindValue(QStringLiteral(":participant"),
                                       QString::fromStdString(projection.selfParticipantId));
                ensureOutbox.bindValue(QStringLiteral(":source"),
                                       optionalVariant(projection.sourceNotificationId));
                ensureOutbox.bindValue(QStringLiteral(":status"),
                                       projection.enqueueDesktopNotification
                                           ? QStringLiteral("pending")
                                           : QStringLiteral("resolved"));
                if (!ensureOutbox.exec())
                {
                    transaction.rollback();
                    return queryError(QStringLiteral("Create calendar invitation outbox"),
                                      ensureOutbox);
                }
                continue;
            }

            removePending.bindValue(QStringLiteral(":account"),
                                    QString::fromStdString(reconciliation.accountId));
            removePending.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
            if (!removePending.exec())
            {
                transaction.rollback();
                return queryError(QStringLiteral("Resolve pending calendar invitation"),
                                  removePending);
            }
            resolveOutbox.bindValue(QStringLiteral(":account"),
                                    QString::fromStdString(reconciliation.accountId));
            resolveOutbox.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
            if (!resolveOutbox.exec())
            {
                transaction.rollback();
                return queryError(QStringLiteral("Resolve calendar invitation outbox"),
                                  resolveOutbox);
            }
            const auto release =
                dispatches.release(transaction, NotificationDispatchKind::Invitation,
                                   invitationKey(reconciliation.accountId, eventId));
            if (release)
            {
                transaction.rollback();
                return release;
            }
        }

        if (const auto error = transaction.commit())
            return error;
        return std::nullopt;
    }

    std::variant<std::vector<std::string>, DatabaseError>
    CalendarInvitationRepository::pendingEventIds(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT event_id FROM calendar_pending_invitations WHERE account_id=:account ORDER BY "
            "event_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("List pending calendar invitation events"), query);
        std::vector<std::string> result;
        while (query.next())
            result.push_back(query.value(0).toString().toStdString());
        return result;
    }

    std::variant<std::vector<CalendarInvitationDispatchCandidate>, DatabaseError>
    CalendarInvitationRepository::claimPendingDispatches()
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Claim calendar invitation notifications"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT o.invitation_key,a.owner_account_id,o.account_id,o.event_id,"
            "o.self_participant_id,o.source_notification_id,e.document_json,"
            "(SELECT c.local_start FROM calendar_occurrences c WHERE c.account_id=o.account_id "
            "AND c.event_id=o.event_id AND substr(c.local_start,1,10)>=:today ORDER BY "
            "c.local_start LIMIT 1),"
            "(SELECT c.start_utc FROM calendar_occurrences c WHERE c.account_id=o.account_id "
            "AND c.event_id=o.event_id AND substr(c.local_start,1,10)>=:today ORDER BY "
            "c.local_start LIMIT 1),"
            "(SELECT c.recurrence_id FROM calendar_occurrences c WHERE c.account_id=o.account_id "
            "AND c.event_id=o.event_id AND substr(c.local_start,1,10)>=:today ORDER BY "
            "c.local_start LIMIT 1) FROM calendar_invitation_outbox o JOIN "
            "calendar_pending_invitations p ON p.account_id=o.account_id AND "
            "p.event_id=o.event_id JOIN calendar_events e ON e.account_id=o.account_id AND "
            "e.event_id=o.event_id JOIN accounts a ON a.account_id=o.account_id WHERE "
            "o.status='pending' ORDER BY o.created_at,o.invitation_key"));
        query.bindValue(QStringLiteral(":today"), QDate::currentDate().toString(Qt::ISODate));
        if (!query.exec())
            return queryError(QStringLiteral("Read calendar invitation outbox"), query);

        NotificationDispatchRepository dispatches{m_connection};
        std::vector<CalendarInvitationDispatchCandidate> result;
        while (query.next())
        {
            const auto key = query.value(0).toString().toStdString();
            const auto claimed =
                dispatches.claim(transaction, NotificationDispatchKind::Invitation, key);
            if (const auto* error = std::get_if<DatabaseError>(&claimed))
            {
                transaction.rollback();
                return *error;
            }
            if (!std::get<bool>(claimed))
                continue;
            const auto accountId = query.value(2).toString().toStdString();
            const auto parsed =
                api::parseCalendarEventDocument(accountId, query.value(6).toString().toStdString());
            if (!parsed.ok())
            {
                transaction.rollback();
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Parse calendar invitation event document")};
            }
            const auto& event = *parsed.value;
            const auto displayStart =
                query.value(7).isNull()
                    ? event.start
                    : calendar::LocalDateTime{.value = query.value(7).toString().toStdString()};
            const auto displayUtc = query.value(8).isNull()
                                        ? event.utcStart
                                        : std::optional<calendar::UtcInstant>{
                                              {.value = query.value(8).toString().toStdString()}};
            const auto recurrenceId = query.value(9).isNull()
                                          ? std::nullopt
                                          : std::optional<calendar::LocalDateTime>{
                                                {.value = query.value(9).toString().toStdString()}};
            result.push_back(CalendarInvitationDispatchCandidate{
                .invitationKey = key,
                .ownerAccountId =
                    query.value(1).isNull() ? accountId : query.value(1).toString().toStdString(),
                .accountId = accountId,
                .eventId = query.value(3).toString().toStdString(),
                .selfParticipantId = query.value(4).toString().toStdString(),
                .sourceNotificationId =
                    query.value(5).isNull()
                        ? std::nullopt
                        : std::optional<std::string>{query.value(5).toString().toStdString()},
                .title = event.title,
                .organizer = organizerName(event),
                .location = event.location,
                .start = displayStart,
                .utcStart = displayUtc,
                .recurrenceId = recurrenceId,
                .allDay = event.showWithoutTime,
                .recurring = event.recurrenceRule.has_value(),
            });
        }
        if (const auto error = transaction.commit())
            return *error;
        return result;
    }

    std::optional<DatabaseError>
    CalendarInvitationRepository::markDelivered(const std::string_view invitationKey,
                                                const QDateTime& deliveredAt)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Deliver calendar invitation"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE calendar_invitation_outbox SET status='delivered',delivered_at=:delivered "
            "WHERE invitation_key=:key AND status='pending'"));
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{invitationKey}));
        query.bindValue(QStringLiteral(":delivered"),
                        deliveredAt.toUTC().toString(Qt::ISODateWithMs));
        if (!query.exec())
            return queryError(QStringLiteral("Mark calendar invitation delivered"), query);
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error = dispatches.release(transaction, NotificationDispatchKind::Invitation,
                                                  invitationKey))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    CalendarInvitationRepository::releaseDispatch(const std::string_view invitationKey)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Release calendar invitation dispatch"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error = dispatches.release(transaction, NotificationDispatchKind::Invitation,
                                                  invitationKey))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError> CalendarInvitationRepository::recoverDispatches()
    {
        NotificationDispatchRepository dispatches{m_connection};
        return dispatches.recover(NotificationDispatchKind::Invitation);
    }

    std::optional<DatabaseError>
    CalendarInvitationRepository::resolveEvent(const std::string_view accountId,
                                               const std::string_view eventId)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Resolve calendar invitation"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery remove{m_connection.database()};
        remove.prepare(
            QStringLiteral("DELETE FROM calendar_pending_invitations WHERE account_id=:account AND "
                           "event_id=:event"));
        remove.bindValue(QStringLiteral(":account"),
                         QString::fromStdString(std::string{accountId}));
        remove.bindValue(QStringLiteral(":event"), QString::fromStdString(std::string{eventId}));
        if (!remove.exec())
            return queryError(QStringLiteral("Remove pending calendar invitation"), remove);
        QSqlQuery resolve{m_connection.database()};
        resolve.prepare(
            QStringLiteral("UPDATE calendar_invitation_outbox SET status='resolved',resolved_at="
                           "COALESCE(resolved_at,CURRENT_TIMESTAMP) WHERE account_id=:account AND "
                           "event_id=:event"));
        resolve.bindValue(QStringLiteral(":account"),
                          QString::fromStdString(std::string{accountId}));
        resolve.bindValue(QStringLiteral(":event"), QString::fromStdString(std::string{eventId}));
        if (!resolve.exec())
            return queryError(QStringLiteral("Resolve calendar invitation notification"), resolve);
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error = dispatches.release(transaction, NotificationDispatchKind::Invitation,
                                                  invitationKey(accountId, eventId)))
            return error;
        return transaction.commit();
    }
} // namespace javelin::jmap::cache
