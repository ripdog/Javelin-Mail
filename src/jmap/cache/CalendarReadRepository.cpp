#include "jmap/cache/CalendarReadRepository.h"

#include "jmap/api/CalendarMethods.h"

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_set>

namespace javelin::jmap::cache
{
    namespace
    {
        [[nodiscard]] DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        [[nodiscard]] calendar::CalendarRights rights(const std::uint32_t mask)
        {
            return {.mayReadFreeBusy = (mask & 1U) != 0U,
                    .mayReadItems = (mask & 2U) != 0U,
                    .mayWriteAll = (mask & 4U) != 0U,
                    .mayWriteOwn = (mask & 8U) != 0U,
                    .mayUpdatePrivate = (mask & 16U) != 0U,
                    .mayRSVP = (mask & 32U) != 0U,
                    .mayShare = (mask & 64U) != 0U,
                    .mayDelete = (mask & 128U) != 0U};
        }
    } // namespace

    CalendarReadRepository::CalendarReadRepository(const DatabaseReadView& connection)
        : m_connection(connection)
    {
    }

    std::variant<std::vector<calendar::Calendar>, DatabaseError>
    CalendarReadRepository::listCalendars(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        const auto& database = m_connection.database();
        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "SELECT c.calendar_id,c.name,c.description,c.color,c.sort_order,c.is_subscribed,"
            "COALESCE(p.is_visible,c.is_visible),c.is_default,c.time_zone,c.rights_json FROM "
            "calendars c LEFT JOIN calendar_preferences p ON p.account_id=c.account_id AND "
            "p.calendar_id=c.calendar_id WHERE c.account_id=:account AND NOT EXISTS (SELECT 1 "
            "FROM calendar_deletion_projections d WHERE d.account_id=c.account_id AND "
            "d.calendar_id=c.calendar_id) ORDER BY c.sort_order,c.name COLLATE "
            "NOCASE,c.calendar_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("List calendars"), query);

        std::vector<calendar::Calendar> result;
        while (query.next())
        {
            calendar::Calendar item{
                .accountId = std::string{accountId},
                .id = query.value(0).toString().toStdString(),
                .name = query.value(1).toString().toStdString(),
                .description = query.value(2).isNull()
                                   ? std::nullopt
                                   : std::optional{query.value(2).toString().toStdString()},
                .color = query.value(3).isNull()
                             ? std::nullopt
                             : std::optional{query.value(3).toString().toStdString()},
                .sortOrder = query.value(4).toUInt(),
                .isSubscribed = query.value(5).toInt() != 0,
                .isVisible = query.value(6).toInt() != 0,
                .isDefault = query.value(7).toInt() != 0,
                .timeZone =
                    query.value(8).isNull()
                        ? std::nullopt
                        : std::optional<calendar::TimeZoneId>{{.value = query.value(8)
                                                                            .toString()
                                                                            .toStdString()}},
                .defaultAlertsWithTime = {},
                .defaultAlertsWithoutTime = {},
                .myRights = rights(query.value(9).toUInt())};
            QSqlQuery alerts{database};
            alerts.prepare(QStringLiteral(
                "SELECT alert_id,without_time,action,trigger_kind,relative_to,offset,trigger_at,"
                "acknowledged FROM calendar_default_alerts WHERE account_id=:account AND "
                "calendar_id=:calendar"));
            alerts.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            alerts.bindValue(QStringLiteral(":calendar"), QString::fromStdString(item.id));
            if (!alerts.exec())
                return queryError(QStringLiteral("List calendar default alerts"), alerts);
            while (alerts.next())
            {
                calendar::Alert alert{
                    .id = alerts.value(0).toString().toStdString(),
                    .action = alerts.value(2).toString().toStdString(),
                    .triggerKind = alerts.value(3).toString() == QStringLiteral("absolute")
                                       ? calendar::AlertTriggerKind::Absolute
                                       : calendar::AlertTriggerKind::Offset,
                    .relativeTo = alerts.value(4).toString().toStdString(),
                    .offset =
                        alerts.value(5).isNull()
                            ? std::nullopt
                            : std::optional<calendar::Duration>{{.value = alerts.value(5)
                                                                              .toString()
                                                                              .toStdString()}},
                    .when =
                        alerts.value(6).isNull()
                            ? std::nullopt
                            : std::optional<calendar::UtcInstant>{{.value = alerts.value(6)
                                                                                .toString()
                                                                                .toStdString()}},
                    .acknowledged = alerts.value(7).isNull()
                                        ? std::nullopt
                                        : std::optional<calendar::UtcInstant>{
                                              {.value = alerts.value(7).toString().toStdString()}}};
                auto& destination = alerts.value(1).toBool() ? item.defaultAlertsWithoutTime
                                                             : item.defaultAlertsWithTime;
                destination.emplace(alert.id, std::move(alert));
            }
            result.push_back(std::move(item));
        }
        return result;
    }

    std::variant<std::vector<CalendarAccount>, DatabaseError>
    CalendarReadRepository::listAccounts() const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        if (!query.exec(QStringLiteral(
                "SELECT owner_account_id,account_id,name FROM accounts WHERE cap_calendars=1 "
                "AND owner_account_id IS NOT NULL ORDER BY is_primary DESC,name COLLATE NOCASE,"
                "account_id")))
            return queryError(QStringLiteral("List calendar accounts"), query);
        std::vector<CalendarAccount> result;
        while (query.next())
            result.push_back({.ownerAccountId = query.value(0).toString().toStdString(),
                              .accountId = query.value(1).toString().toStdString(),
                              .name = query.value(2).toString().toStdString()});
        return result;
    }

    std::variant<std::optional<std::string>, DatabaseError>
    CalendarReadRepository::stateToken(const std::string_view accountId,
                                       const std::string_view dataType) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (dataType != "Calendar" && dataType != "CalendarEvent")
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Unsupported calendar state type")};
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral("SELECT state FROM calendar_state_tokens WHERE "
                                     "account_id=:account AND data_type=:type"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":type"), QString::fromStdString(std::string{dataType}));
        if (!query.exec())
            return queryError(QStringLiteral("Load calendar state"), query);
        if (!query.next())
            return std::optional<std::string>{std::nullopt};
        return std::optional{query.value(0).toString().toStdString()};
    }

    std::variant<std::optional<calendar::CalendarEvent>, DatabaseError>
    CalendarReadRepository::findEvent(const std::string_view accountId,
                                      const std::string_view eventId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT document_json FROM calendar_events WHERE account_id=:account AND "
            "event_id=:event"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":event"), QString::fromStdString(std::string{eventId}));
        if (!query.exec())
            return queryError(QStringLiteral("Find calendar event"), query);
        if (!query.next())
            return std::optional<calendar::CalendarEvent>{std::nullopt};
        const auto parsed =
            api::parseCalendarEventDocument(accountId, query.value(0).toString().toStdString());
        if (!parsed.ok() || !parsed.value.has_value())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message =
                                     QStringLiteral("Cached calendar event document is invalid")};
        return std::optional<calendar::CalendarEvent>{std::move(*parsed.value)};
    }

    std::variant<std::optional<CalendarWindow>, DatabaseError> CalendarReadRepository::loadWindow(
        const std::string_view accountId, const calendar::LocalDateTime& start,
        const calendar::LocalDateTime& end, const calendar::TimeZoneId& displayTimeZone) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        const auto& database = m_connection.database();
        QSqlQuery windowQuery{database};
        windowQuery.prepare(QStringLiteral(
            "SELECT w.query_state,(SELECT state FROM calendar_state_tokens s WHERE "
            "s.account_id=w.account_id AND s.data_type='CalendarEvent') FROM "
            "calendar_query_windows w WHERE w.account_id=:account AND w.range_start=:start AND "
            "w.range_end=:end AND w.display_time_zone=:zone"));
        windowQuery.bindValue(QStringLiteral(":account"),
                              QString::fromStdString(std::string{accountId}));
        windowQuery.bindValue(QStringLiteral(":start"), QString::fromStdString(start.value));
        windowQuery.bindValue(QStringLiteral(":end"), QString::fromStdString(end.value));
        windowQuery.bindValue(QStringLiteral(":zone"),
                              QString::fromStdString(displayTimeZone.value));
        if (!windowQuery.exec())
            return queryError(QStringLiteral("Load calendar window"), windowQuery);
        if (!windowQuery.next())
            return std::optional<CalendarWindow>{std::nullopt};

        CalendarWindow result{.accountId = std::string{accountId},
                              .start = start,
                              .end = end,
                              .displayTimeZone = displayTimeZone,
                              .queryState = windowQuery.value(0).toString().toStdString(),
                              .eventState = windowQuery.value(1).toString().toStdString(),
                              .events = {},
                              .occurrences = {}};
        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "SELECT o.occurrence_id,o.event_id,o.recurrence_id,o.start_utc,o.end_utc,o.local_start,"
            "o.local_end,o.is_all_day,e.document_json FROM calendar_window_occurrences w JOIN "
            "calendar_occurrences o ON o.account_id=w.account_id AND "
            "o.occurrence_id=w.occurrence_id JOIN calendar_events e ON e.account_id=o.account_id "
            "AND e.event_id=o.event_id WHERE w.account_id=:account AND w.range_start=:start AND "
            "w.range_end=:end AND w.display_time_zone=:zone ORDER BY o.local_start,o.local_end,"
            "o.occurrence_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":start"), QString::fromStdString(start.value));
        query.bindValue(QStringLiteral(":end"), QString::fromStdString(end.value));
        query.bindValue(QStringLiteral(":zone"), QString::fromStdString(displayTimeZone.value));
        if (!query.exec())
            return queryError(QStringLiteral("Load calendar occurrences"), query);

        std::unordered_set<std::string> loadedEvents;
        while (query.next())
        {
            const auto eventId = query.value(1).toString().toStdString();
            result.occurrences.push_back(calendar::Occurrence{
                .accountId = std::string{accountId},
                .id = query.value(0).toString().toStdString(),
                .eventId = eventId,
                .recurrenceId =
                    query.value(2).isNull()
                        ? std::nullopt
                        : std::optional<calendar::LocalDateTime>{{.value = query.value(2)
                                                                               .toString()
                                                                               .toStdString()}},
                .localStart = {.value = query.value(5).toString().toStdString()},
                .localEnd = {.value = query.value(6).toString().toStdString()},
                .utcStart =
                    query.value(3).isNull()
                        ? std::nullopt
                        : std::optional<calendar::UtcInstant>{{.value = query.value(3)
                                                                            .toString()
                                                                            .toStdString()}},
                .utcEnd = query.value(4).isNull()
                              ? std::nullopt
                              : std::optional<calendar::UtcInstant>{{.value = query.value(4)
                                                                                  .toString()
                                                                                  .toStdString()}},
                .allDay = query.value(7).toInt() != 0});
            if (loadedEvents.insert(eventId).second)
            {
                const auto parsed = api::parseCalendarEventDocument(
                    accountId, query.value(8).toString().toStdString());
                if (!parsed.ok() || !parsed.value.has_value())
                    return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                         .message = QStringLiteral("Parse cached calendar event")};
                result.events.push_back(*parsed.value);
            }
        }
        return std::optional<CalendarWindow>{std::move(result)};
    }
} // namespace javelin::jmap::cache
