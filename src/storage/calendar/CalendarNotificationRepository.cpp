#include "jmap/cache/CalendarNotificationRepository.h"

#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/NotificationDispatchRepository.h"

#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>

#include <cmath>

namespace javelin::jmap::cache
{
    namespace
    {
        DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        std::optional<qint64> durationSeconds(const std::string& value)
        {
            static const QRegularExpression expression{
                QStringLiteral("^([+-])?P(?:(\\d+)W)?(?:(\\d+)D)?(?:T(?:(\\d+)H)?(?:(\\d+)M)?"
                               "(?:(\\d+(?:\\.\\d+)?)S)?)?$")};
            const auto match = expression.match(QString::fromStdString(value));
            if (!match.hasMatch())
                return std::nullopt;
            const qint64 seconds = match.captured(2).toLongLong() * 7 * 86400 +
                                   match.captured(3).toLongLong() * 86400 +
                                   match.captured(4).toLongLong() * 3600 +
                                   match.captured(5).toLongLong() * 60 +
                                   static_cast<qint64>(std::round(match.captured(6).toDouble()));
            return match.captured(1) == QStringLiteral("-") ? -seconds : seconds;
        }

        QDateTime instant(const std::string& value)
        {
            return QDateTime::fromString(QString::fromStdString(value), Qt::ISODateWithMs).toUTC();
        }
    } // namespace

    CalendarNotificationRepository::CalendarNotificationRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
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
            "SELECT o.account_id,o.occurrence_id,o.event_id,o.start_utc,o.end_utc,e.title,"
            "e.document_json,a.owner_account_id FROM calendar_occurrences o JOIN calendar_events "
            "e ON e.account_id=o.account_id AND e.event_id=o.event_id JOIN accounts a ON "
            "a.account_id=o.account_id WHERE EXISTS (SELECT 1 FROM "
            "calendar_event_calendars ec JOIN calendars c ON c.account_id=ec.account_id AND "
            "c.calendar_id=ec.calendar_id WHERE ec.account_id=o.account_id AND "
            "ec.event_id=o.event_id AND c.is_subscribed=1) ORDER BY o.start_utc"));
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
                QDateTime trigger;
                if (alert.triggerKind == calendar::AlertTriggerKind::Absolute && alert.when)
                    trigger = instant(alert.when->value);
                else if (alert.offset)
                {
                    const auto offset = durationSeconds(alert.offset->value);
                    if (!offset)
                        continue;
                    trigger = (alert.relativeTo == "end" ? endsAt : startsAt).addSecs(*offset);
                }
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
                state.bindValue(QStringLiteral(":key"), QString::fromStdString(key));
                if (!state.exec())
                {
                    database.rollback();
                    return queryError(QStringLiteral("Read calendar notification state"), state);
                }
                bool due = true;
                if (state.next())
                {
                    const QString status = state.value(0).toString();
                    due =
                        status == QStringLiteral("snoozed") &&
                        QDateTime::fromString(state.value(1).toString(), Qt::ISODateWithMs) <= now;
                }
                if (!due)
                    continue;
                const auto claimed =
                    dispatches.claim(transaction, NotificationDispatchKind::Calendar, key);
                if (const auto* error = std::get_if<DatabaseError>(&claimed))
                    return *error;
                if (!std::get<bool>(claimed))
                    continue;
                candidates.push_back({.key = key,
                                      .ownerAccountId = query.value(7).toString().toStdString(),
                                      .accountId = accountId,
                                      .eventId = eventId,
                                      .occurrenceId = occurrenceId,
                                      .alertId = alertId,
                                      .title = query.value(5).toString().toStdString(),
                                      .startsAt = startsAt,
                                      .alert = alert});
            }
        }
        if (const auto error = transaction.commit())
            return *error;
        return candidates;
    }

    std::optional<DatabaseError>
    CalendarNotificationRepository::markDelivered(const std::string_view key,
                                                  const QDateTime& deliveredAt)
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
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{key}));
        query.bindValue(QStringLiteral(":now"), deliveredAt.toUTC().toString(Qt::ISODateWithMs));
        if (!query.exec())
            return queryError(QStringLiteral("Record calendar notification delivery"), query);
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error =
                dispatches.release(transaction, NotificationDispatchKind::Calendar, key))
            return error;
        return transaction.commit();
    }

    std::optional<DatabaseError>
    CalendarNotificationRepository::releaseDispatch(const std::string_view key)
    {
        auto transactionResult = DatabaseTransaction::begin(
            m_connection, QStringLiteral("Release calendar notification delivery"));
        if (const auto* error = std::get_if<DatabaseError>(&transactionResult))
            return *error;
        auto transaction = std::get<DatabaseTransaction>(std::move(transactionResult));
        NotificationDispatchRepository dispatches{m_connection};
        if (const auto error =
                dispatches.release(transaction, NotificationDispatchKind::Calendar, key))
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

    std::optional<DatabaseError> CalendarNotificationRepository::snooze(const std::string_view key,
                                                                        const QDateTime& until)
    {
        const DatabaseWriteScope writeScope{m_connection};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "UPDATE calendar_notification_state SET status='snoozed',snoozed_until=:until "
            "WHERE notification_key=:key"));
        query.bindValue(QStringLiteral(":until"), until.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(QStringLiteral(":key"), QString::fromStdString(std::string{key}));
        return query.exec()
                   ? std::nullopt
                   : std::optional{queryError(QStringLiteral("Snooze calendar reminder"), query)};
    }
} // namespace javelin::jmap::cache
