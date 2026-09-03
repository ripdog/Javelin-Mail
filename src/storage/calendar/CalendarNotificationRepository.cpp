#include "jmap/cache/CalendarNotificationRepository.h"

#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/NotificationDispatchRepository.h"
#include "jmap/calendar/CalendarEventEditing.h"

#include <QSqlError>
#include <QSqlQuery>

namespace javelin::jmap::cache
{
    namespace
    {
        DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        QDateTime instant(const std::string& value)
        {
            return QDateTime::fromString(QString::fromStdString(value), Qt::ISODateWithMs).toUTC();
        }
    } // namespace

    std::string calendarNotificationIdentityKey(const std::string_view accountId,
                                                const std::string_view eventId,
                                                const std::optional<std::string>& recurrenceId,
                                                const std::string_view alertId,
                                                const QDateTime& trigger)
    {
        const auto append = [](std::string& key, const std::string_view value)
        {
            key += std::to_string(value.size());
            key.push_back(':');
            key.append(value);
        };
        std::string key{"calendar:"};
        append(key, accountId);
        append(key, eventId);
        append(key, recurrenceId ? std::string_view{*recurrenceId} : std::string_view{});
        append(key, alertId);
        append(key, std::to_string(trigger.toMSecsSinceEpoch()));
        return key;
    }

    CalendarNotificationRepository::CalendarNotificationRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::variant<bool, DatabaseError>
    CalendarNotificationRepository::enqueuePushed(const CalendarPushedAlert& alert)
    {
        const DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO calendar_pushed_alerts(push_key,owner_account_id,account_id,"
            "event_id,uid,recurrence_id,alert_id) VALUES "
            "(:key,:owner,:account,:event,:uid,:recurrence,:alert)"));
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(alert.key));
        query.bindValue(QStringLiteral(":owner"), QString::fromStdString(alert.ownerAccountId));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(alert.accountId));
        query.bindValue(QStringLiteral(":event"), QString::fromStdString(alert.eventId));
        query.bindValue(QStringLiteral(":uid"), QString::fromStdString(alert.uid));
        query.bindValue(QStringLiteral(":recurrence"),
                        alert.recurrenceId ? QVariant{QString::fromStdString(*alert.recurrenceId)}
                                           : QVariant{});
        query.bindValue(QStringLiteral(":alert"), QString::fromStdString(alert.alertId));
        if (!query.exec())
            return queryError(QStringLiteral("Queue pushed calendar alert"), query);
        return query.numRowsAffected() > 0;
    }

    std::variant<std::vector<CalendarPushedAlert>, DatabaseError>
    CalendarNotificationRepository::pendingPushed(
        const std::optional<std::string_view> ownerAccountId) const
    {
        QSqlQuery query{m_connection.database()};
        query.prepare(
            ownerAccountId
                ? QStringLiteral("SELECT push_key,owner_account_id,account_id,event_id,uid,"
                                 "recurrence_id,alert_id FROM calendar_pushed_alerts WHERE "
                                 "owner_account_id=:owner ORDER BY received_at,push_key")
                : QStringLiteral("SELECT push_key,owner_account_id,account_id,event_id,uid,"
                                 "recurrence_id,alert_id FROM calendar_pushed_alerts ORDER BY "
                                 "received_at,push_key"));
        if (ownerAccountId)
            query.bindValue(QStringLiteral(":owner"),
                            QString::fromStdString(std::string{*ownerAccountId}));
        if (!query.exec())
            return queryError(QStringLiteral("Read queued calendar alerts"), query);
        std::vector<CalendarPushedAlert> alerts;
        while (query.next())
        {
            alerts.push_back({
                .key = query.value(0).toString().toStdString(),
                .ownerAccountId = query.value(1).toString().toStdString(),
                .accountId = query.value(2).toString().toStdString(),
                .eventId = query.value(3).toString().toStdString(),
                .uid = query.value(4).toString().toStdString(),
                .recurrenceId = query.value(5).isNull()
                                    ? std::nullopt
                                    : std::optional{query.value(5).toString().toStdString()},
                .alertId = query.value(6).toString().toStdString(),
            });
        }
        return alerts;
    }

    std::optional<DatabaseError>
    CalendarNotificationRepository::removePushed(const std::string_view key)
    {
        const DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("DELETE FROM calendar_pushed_alerts WHERE push_key=:key"));
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{key}));
        return query.exec() ? std::nullopt
                            : std::optional{queryError(
                                  QStringLiteral("Remove queued calendar alert"), query)};
    }

    std::variant<CalendarPushedAlertClaim, DatabaseError>
    CalendarNotificationRepository::claimPushed(const std::string_view key, const QDateTime& now)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Claim pushed calendar notification"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery state{m_connection.database()};
        state.prepare(
            QStringLiteral("SELECT status,snoozed_until FROM calendar_notification_state WHERE "
                           "notification_key=:key"));
        state.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{key}));
        if (!state.exec())
            return queryError(QStringLiteral("Read pushed calendar notification state"), state);
        if (state.next())
        {
            const auto status = state.value(0).toString();
            if (status == QStringLiteral("snoozed") && !state.value(1).isNull())
            {
                const auto retryAt =
                    QDateTime::fromString(state.value(1).toString(), Qt::ISODateWithMs).toUTC();
                if (retryAt.isValid() && retryAt > now)
                {
                    transaction.rollback();
                    return CalendarPushedAlertClaim{
                        .claimed = false, .completed = false, .retryAt = retryAt};
                }
            }
            else
            {
                transaction.rollback();
                return CalendarPushedAlertClaim{
                    .claimed = false, .completed = true, .retryAt = std::nullopt};
            }
        }

        NotificationDispatchRepository dispatches{m_connection};
        const auto claimed = dispatches.claim(transaction, NotificationDispatchKind::Calendar, key);
        if (const auto* error = std::get_if<DatabaseError>(&claimed))
            return *error;
        if (!std::get<bool>(claimed))
        {
            transaction.rollback();
            return CalendarPushedAlertClaim{};
        }
        if (const auto error = transaction.commit())
            return *error;
        return CalendarPushedAlertClaim{
            .claimed = true, .completed = false, .retryAt = std::nullopt};
    }

    std::variant<CalendarNotificationEligibility, DatabaseError>
    CalendarNotificationRepository::notificationEligibility(
        const std::string_view accountId, const calendar::CalendarEvent& event) const
    {
        QSqlQuery metadata{m_connection.database()};
        metadata.prepare(
            QStringLiteral("SELECT 1 FROM calendar_state_tokens WHERE account_id=:account AND "
                           "data_type='Calendar'"));
        metadata.bindValue(QStringLiteral(":account"),
                           QString::fromStdString(std::string{accountId}));
        if (!metadata.exec())
            return queryError(QStringLiteral("Read calendar reminder metadata state"), metadata);
        const bool metadataAvailable = metadata.next();

        QSqlQuery calendar{m_connection.database()};
        calendar.prepare(QStringLiteral("SELECT is_subscribed FROM calendars WHERE "
                                        "account_id=:account AND calendar_id=:calendar"));
        bool hasCalendar = false;
        for (const auto& [calendarId, present] : event.calendarIds)
        {
            if (!present)
                continue;
            hasCalendar = true;
            calendar.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(std::string{accountId}));
            calendar.bindValue(QStringLiteral(":calendar"), QString::fromStdString(calendarId));
            if (!calendar.exec())
                return queryError(QStringLiteral("Read calendar reminder eligibility"), calendar);
            if (!calendar.next())
            {
                if (!metadataAvailable)
                    return CalendarNotificationEligibility::MetadataMissing;
                continue;
            }
            if (calendar.value(0).toInt() != 0)
                return CalendarNotificationEligibility::Eligible;
        }
        if (!hasCalendar)
            return CalendarNotificationEligibility::NotEligible;
        return metadataAvailable ? CalendarNotificationEligibility::NotEligible
                                 : CalendarNotificationEligibility::MetadataMissing;
    }

    std::variant<std::optional<calendar::Alert>, DatabaseError>
    CalendarNotificationRepository::effectiveAlert(const std::string_view accountId,
                                                   const calendar::CalendarEvent& event,
                                                   const std::string_view alertId) const
    {
        if (!event.useDefaultAlerts)
        {
            const auto found = event.alerts.find(std::string{alertId});
            return std::optional<calendar::Alert>{
                found == event.alerts.end() ? std::nullopt : std::optional{found->second}};
        }

        QSqlQuery defaults{m_connection.database()};
        defaults.prepare(QStringLiteral(
            "SELECT d.action,d.trigger_kind,d.relative_to,d.offset,d.trigger_at,d.acknowledged "
            "FROM calendar_default_alerts d JOIN calendars c ON c.account_id=d.account_id AND "
            "c.calendar_id=d.calendar_id WHERE d.account_id=:account AND d.calendar_id=:calendar "
            "AND c.is_subscribed=1 AND d.alert_id=:alert AND d.without_time=:without_time LIMIT "
            "1"));
        bool foundDefault = false;
        for (const auto& [calendarId, present] : event.calendarIds)
        {
            if (!present)
                continue;
            defaults.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(std::string{accountId}));
            defaults.bindValue(QStringLiteral(":calendar"), QString::fromStdString(calendarId));
            defaults.bindValue(QStringLiteral(":alert"),
                               QString::fromStdString(std::string{alertId}));
            defaults.bindValue(QStringLiteral(":without_time"), event.showWithoutTime ? 1 : 0);
            if (!defaults.exec())
                return queryError(QStringLiteral("Read pushed calendar default alert"), defaults);
            if (defaults.next())
            {
                foundDefault = true;
                break;
            }
        }
        if (!foundDefault)
            return std::optional<calendar::Alert>{std::nullopt};

        calendar::Alert alert{
            .id = std::string{alertId},
            .action = defaults.value(0).toString().toStdString(),
            .triggerKind = defaults.value(1).toString() == QStringLiteral("absolute")
                               ? calendar::AlertTriggerKind::Absolute
                               : calendar::AlertTriggerKind::Offset,
            .relativeTo = defaults.value(2).toString().toStdString(),
            .offset = defaults.value(3).isNull()
                          ? std::nullopt
                          : std::optional<calendar::Duration>{{.value = defaults.value(3)
                                                                            .toString()
                                                                            .toStdString()}},
            .when = defaults.value(4).isNull()
                        ? std::nullopt
                        : std::optional<calendar::UtcInstant>{{.value = defaults.value(4)
                                                                            .toString()
                                                                            .toStdString()}},
            .acknowledged =
                defaults.value(5).isNull()
                    ? std::nullopt
                    : std::optional<calendar::UtcInstant>{{.value = defaults.value(5)
                                                                        .toString()
                                                                        .toStdString()}},
        };
        if (const auto overridden = event.alerts.find(std::string{alertId});
            overridden != event.alerts.end() && overridden->second.acknowledged)
            alert.acknowledged = overridden->second.acknowledged;
        return std::optional{std::move(alert)};
    }

    std::variant<std::vector<CalendarNotificationCandidate>, DatabaseError>
    CalendarNotificationRepository::claimDue(const QDateTime& now)
    {
        m_nextTrigger = std::nullopt;
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Claim due calendar notifications"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        auto& database = m_connection.database();

        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "SELECT o.account_id,o.occurrence_id,o.event_id,r.start_utc,r.end_utc,e.title,"
            "e.document_json,a.owner_account_id,o.recurrence_id FROM calendar_reminder_occurrences "
            "r JOIN calendar_occurrences o ON o.account_id=r.account_id AND "
            "o.occurrence_id=r.occurrence_id JOIN calendar_events e ON e.account_id=o.account_id "
            "AND e.event_id=o.event_id JOIN accounts a ON a.account_id=o.account_id WHERE EXISTS "
            "(SELECT 1 FROM calendar_event_calendars ec JOIN calendars c ON "
            "c.account_id=ec.account_id AND c.calendar_id=ec.calendar_id WHERE "
            "ec.account_id=o.account_id AND ec.event_id=o.event_id AND c.is_subscribed=1) ORDER BY "
            "r.start_utc"));
        if (!query.exec())
        {
            database.rollback();
            return queryError(QStringLiteral("Find calendar notification events"), query);
        }

        QSqlQuery state{database};
        state.prepare(
            QStringLiteral("SELECT status,snoozed_until FROM calendar_notification_state WHERE "
                           "notification_key=:key"));
        NotificationDispatchRepository dispatches{m_connection};
        std::vector<CalendarNotificationCandidate> candidates;
        const auto oldestTrigger = now.addDays(-1);
        while (query.next())
        {
            const std::string accountId = query.value(0).toString().toStdString();
            const std::string occurrenceId = query.value(1).toString().toStdString();
            const std::string eventId = query.value(2).toString().toStdString();
            const auto recurrenceId = query.value(8).isNull()
                                          ? std::optional<std::string>{}
                                          : std::optional{query.value(8).toString().toStdString()};
            const auto parsed =
                api::parseCalendarEventDocument(accountId, query.value(6).toString().toStdString());
            if (!parsed.ok() || !parsed.value || parsed.value->isDraft)
                continue;
            const QDateTime startsAt =
                QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toUTC();
            const QDateTime endsAt =
                QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs).toUTC();
            auto effectiveAlerts = parsed.value->alerts;
            if (parsed.value->useDefaultAlerts)
            {
                std::unordered_map<std::string, calendar::Alert> defaultAlerts;
                QSqlQuery defaults{database};
                defaults.prepare(QStringLiteral(
                    "SELECT d.alert_id,d.action,d.trigger_kind,d.relative_to,d.offset,"
                    "d.trigger_at,d.acknowledged FROM calendar_default_alerts d JOIN "
                    "calendar_event_calendars ec ON ec.account_id=d.account_id AND "
                    "ec.calendar_id=d.calendar_id JOIN calendars c ON c.account_id=d.account_id "
                    "AND c.calendar_id=d.calendar_id WHERE ec.account_id=:account AND "
                    "ec.event_id=:event AND c.is_subscribed=1 AND d.without_time=:without_time"));
                defaults.bindValue(QStringLiteral(":account"), QString::fromStdString(accountId));
                defaults.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
                defaults.bindValue(QStringLiteral(":without_time"),
                                   parsed.value->showWithoutTime ? 1 : 0);
                if (!defaults.exec())
                {
                    database.rollback();
                    return queryError(QStringLiteral("Read default calendar alerts"), defaults);
                }
                while (defaults.next())
                {
                    const std::string alertId = defaults.value(0).toString().toStdString();
                    calendar::Alert alert{
                        .id = alertId,
                        .action = defaults.value(1).toString().toStdString(),
                        .triggerKind = defaults.value(2).toString() == QStringLiteral("absolute")
                                           ? calendar::AlertTriggerKind::Absolute
                                           : calendar::AlertTriggerKind::Offset,
                        .relativeTo = defaults.value(3).toString().toStdString(),
                        .offset =
                            defaults.value(4).isNull()
                                ? std::nullopt
                                : std::optional<calendar::Duration>{{.value = defaults.value(4)
                                                                                  .toString()
                                                                                  .toStdString()}},
                        .when = defaults.value(5).isNull()
                                    ? std::nullopt
                                    : std::optional<calendar::UtcInstant>{{.value =
                                                                               defaults.value(5)
                                                                                   .toString()
                                                                                   .toStdString()}},
                        .acknowledged =
                            defaults.value(6).isNull()
                                ? std::nullopt
                                : std::optional<calendar::UtcInstant>{
                                      {.value = defaults.value(6).toString().toStdString()}}};
                    if (const auto overridden = parsed.value->alerts.find(alertId);
                        overridden != parsed.value->alerts.end() && overridden->second.acknowledged)
                        alert.acknowledged = overridden->second.acknowledged;
                    defaultAlerts.emplace(alertId, std::move(alert));
                }
                if (!defaultAlerts.empty())
                    effectiveAlerts = std::move(defaultAlerts);
            }
            for (const auto& [alertId, alert] : effectiveAlerts)
            {
                if (alert.action != "display")
                    continue;
                const QDateTime trigger = calendar::alertTrigger(alert, startsAt, endsAt);
                if (!trigger.isValid() || trigger < oldestTrigger)
                    continue;
                if (alert.acknowledged && instant(alert.acknowledged->value) >= trigger)
                    continue;
                if (trigger > now)
                {
                    if (!m_nextTrigger || trigger < *m_nextTrigger)
                        m_nextTrigger = trigger;
                    continue;
                }

                const std::string key =
                    accountId + ":" +
                    (alert.triggerKind == calendar::AlertTriggerKind::Absolute ? eventId
                                                                               : occurrenceId) +
                    ":" + alertId + ":" + std::to_string(trigger.toMSecsSinceEpoch());
                const auto identityRecurrence =
                    alert.triggerKind == calendar::AlertTriggerKind::Absolute
                        ? std::optional<std::string>{}
                        : recurrenceId;
                const std::string identityKey = calendarNotificationIdentityKey(
                    accountId, eventId, identityRecurrence, alertId, trigger);
                state.bindValue(QStringLiteral(":key"), QString::fromStdString(key));
                if (!state.exec())
                {
                    database.rollback();
                    return queryError(QStringLiteral("Read calendar notification state"), state);
                }
                bool due = true;
                const bool legacyStateExists = state.next();
                if (legacyStateExists)
                {
                    const QString status = state.value(0).toString();
                    due =
                        status == QStringLiteral("snoozed") &&
                        QDateTime::fromString(state.value(1).toString(), Qt::ISODateWithMs) <= now;
                    QSqlQuery alias{database};
                    alias.prepare(QStringLiteral(
                        "INSERT OR IGNORE INTO calendar_notification_state(notification_key,status,"
                        "notified_at,snoozed_until) SELECT "
                        ":identity,status,notified_at,snoozed_until "
                        "FROM calendar_notification_state WHERE notification_key=:legacy"));
                    alias.bindValue(QStringLiteral(":identity"),
                                    QString::fromStdString(identityKey));
                    alias.bindValue(QStringLiteral(":legacy"), QString::fromStdString(key));
                    if (!alias.exec())
                    {
                        database.rollback();
                        return queryError(QStringLiteral("Alias calendar notification state"),
                                          alias);
                    }
                }
                else
                {
                    state.bindValue(QStringLiteral(":key"), QString::fromStdString(identityKey));
                    if (!state.exec())
                    {
                        database.rollback();
                        return queryError(
                            QStringLiteral("Read calendar notification identity state"), state);
                    }
                    if (state.next())
                    {
                        const QString status = state.value(0).toString();
                        due = status == QStringLiteral("snoozed") &&
                              QDateTime::fromString(state.value(1).toString(), Qt::ISODateWithMs) <=
                                  now;
                    }
                }
                if (!due)
                    continue;
                const auto claimed =
                    dispatches.claim(transaction, NotificationDispatchKind::Calendar, identityKey);
                if (const auto* error = std::get_if<DatabaseError>(&claimed))
                    return *error;
                if (!std::get<bool>(claimed))
                    continue;
                candidates.push_back({.key = key,
                                      .identityKey = identityKey,
                                      .ownerAccountId = query.value(7).toString().toStdString(),
                                      .accountId = accountId,
                                      .eventId = eventId,
                                      .occurrenceId = occurrenceId,
                                      .recurrenceId = recurrenceId,
                                      .alertId = alertId,
                                      .title = query.value(5).toString().toStdString(),
                                      .startsAt = startsAt,
                                      .alert = alert,
                                      .pushedAlert = std::nullopt});
            }
        }
        if (const auto error = transaction.commit())
            return *error;
        return candidates;
    }

    std::optional<DatabaseError> CalendarNotificationRepository::markDelivered(
        const std::string_view key, const std::string_view identityKey,
        const QDateTime& deliveredAt, const std::optional<std::string_view> pushedAlertKey)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Complete calendar notification delivery"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO calendar_notification_state(notification_key,status,notified_at) "
            "VALUES(:key,'notified',:now) ON CONFLICT(notification_key) DO UPDATE SET "
            "status='notified',notified_at=excluded.notified_at,snoozed_until=NULL"));
        query.bindValue(QStringLiteral(":now"), deliveredAt.toUTC().toString(Qt::ISODateWithMs));
        for (const auto stateKey : {key, identityKey})
        {
            query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{stateKey}));
            if (!query.exec())
                return queryError(QStringLiteral("Record calendar notification delivery"), query);
            if (identityKey == key)
                break;
        }
        if (pushedAlertKey)
        {
            QSqlQuery pushed{m_connection.database()};
            pushed.prepare(
                QStringLiteral("DELETE FROM calendar_pushed_alerts WHERE push_key=:key"));
            pushed.bindValue(QStringLiteral(":key"),
                             QString::fromStdString(std::string{*pushedAlertKey}));
            if (!pushed.exec())
                return queryError(QStringLiteral("Complete queued calendar alert"), pushed);
        }
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error =
                dispatches.release(transaction, NotificationDispatchKind::Calendar, identityKey))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    CalendarNotificationRepository::releaseDispatch(const std::string_view identityKey)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Release calendar notification delivery"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error =
                dispatches.release(transaction, NotificationDispatchKind::Calendar, identityKey))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError> CalendarNotificationRepository::recoverDispatches()
    {
        NotificationDispatchRepository dispatches{m_connection};
        return dispatches.recover(NotificationDispatchKind::Calendar);
    }

    std::optional<QDateTime> CalendarNotificationRepository::nextTrigger() const
    {
        return m_nextTrigger;
    }

    std::optional<DatabaseError> CalendarNotificationRepository::dismiss(const std::string_view key)
    {
        const DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE calendar_notification_state SET status='dismissed',snoozed_until=NULL "
            "WHERE notification_key=:key"));
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{key}));
        return query.exec()
                   ? std::nullopt
                   : std::optional{queryError(QStringLiteral("Dismiss calendar reminder"), query)};
    }

    std::optional<DatabaseError>
    CalendarNotificationRepository::snooze(const std::string_view key, const QDateTime& until,
                                           const std::optional<CalendarPushedAlert>& pushedAlert)
    {
        auto transactionResult =
            DatabaseTransaction::begin(m_connection, QStringLiteral("Snooze calendar reminder"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE calendar_notification_state SET status='snoozed',snoozed_until=:until "
            "WHERE notification_key=:key"));
        query.bindValue(QStringLiteral(":until"), until.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{key}));
        if (!query.exec())
            return queryError(QStringLiteral("Snooze calendar reminder"), query);

        if (pushedAlert)
        {
            QSqlQuery pushed{m_connection.database()};
            pushed.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO calendar_pushed_alerts(push_key,owner_account_id,account_id,"
                "event_id,uid,recurrence_id,alert_id) VALUES "
                "(:push_key,:owner,:account,:event,:uid,:recurrence,:alert)"));
            pushed.bindValue(QStringLiteral(":push_key"), QString::fromStdString(pushedAlert->key));
            pushed.bindValue(QStringLiteral(":owner"),
                             QString::fromStdString(pushedAlert->ownerAccountId));
            pushed.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(pushedAlert->accountId));
            pushed.bindValue(QStringLiteral(":event"),
                             QString::fromStdString(pushedAlert->eventId));
            pushed.bindValue(QStringLiteral(":uid"), QString::fromStdString(pushedAlert->uid));
            pushed.bindValue(QStringLiteral(":recurrence"),
                             pushedAlert->recurrenceId
                                 ? QVariant{QString::fromStdString(*pushedAlert->recurrenceId)}
                                 : QVariant{});
            pushed.bindValue(QStringLiteral(":alert"),
                             QString::fromStdString(pushedAlert->alertId));
            if (!pushed.exec())
                return queryError(QStringLiteral("Queue snoozed calendar alert"), pushed);
        }
        return transaction.commit();
    }
} // namespace javelin::jmap::cache
