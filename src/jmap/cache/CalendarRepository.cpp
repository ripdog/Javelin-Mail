#include "jmap/cache/CalendarRepository.h"

#include "jmap/api/CalendarMethods.h"

#include <QSqlError>
#include <QSqlQuery>

#include <unordered_set>

namespace javelin::jmap::cache
{
    namespace
    {
        DatabaseError queryError(const QString& operation, const QSqlQuery& query)
        {
            return {.code = DatabaseErrorCode::QueryFailed,
                    .message = operation + QStringLiteral(": ") + query.lastError().text()};
        }

        std::uint32_t rightsMask(const calendar::CalendarRights& rights)
        {
            return (rights.mayReadFreeBusy ? 1U : 0U) | (rights.mayReadItems ? 2U : 0U) |
                   (rights.mayWriteAll ? 4U : 0U) | (rights.mayWriteOwn ? 8U : 0U) |
                   (rights.mayUpdatePrivate ? 16U : 0U) | (rights.mayRSVP ? 32U : 0U) |
                   (rights.mayShare ? 64U : 0U) | (rights.mayDelete ? 128U : 0U);
        }

        calendar::CalendarRights rights(const std::uint32_t mask)
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

        QVariant optionalString(const std::optional<std::string>& value)
        {
            return value ? QVariant{QString::fromStdString(*value)} : QVariant{};
        }

        bool exec(QSqlQuery& query, std::optional<DatabaseError>& failure, const QString& operation)
        {
            if (query.exec())
                return true;
            failure = queryError(operation, query);
            return false;
        }
    } // namespace

    CalendarRepository::CalendarRepository(DatabaseConnection& connection)
        : m_connection(connection)
    {
    }

    std::optional<DatabaseError>
    CalendarRepository::replaceCalendars(const std::string_view accountId,
                                         const std::string_view state,
                                         const std::vector<calendar::Calendar>& calendars)
    {
        if (const auto error = m_connection.validate())
            return error;
        auto& database = m_connection.database();
        if (!database.transaction())
        {
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin calendar replacement: ") +
                                            database.lastError().text()};
        }
        std::optional<DatabaseError> failure;
        QSqlQuery upsert{database};
        upsert.prepare(QStringLiteral(
            "INSERT INTO calendars (account_id,calendar_id,name,description,color,sort_order,"
            "is_subscribed,is_visible,is_default,time_zone,rights_json,state) VALUES "
            "(:account,:id,:name,:description,:color,:sort,:subscribed,:visible,:default,"
            ":time_zone,:rights,:state) ON CONFLICT(account_id,calendar_id) DO UPDATE SET "
            "name=excluded.name,description=excluded.description,color=excluded.color,"
            "sort_order=excluded.sort_order,is_subscribed=excluded.is_subscribed,"
            "is_visible=excluded.is_visible,is_default=excluded.is_default,"
            "time_zone=excluded.time_zone,rights_json=excluded.rights_json,state=excluded.state"));
        std::unordered_set<std::string> retained;
        for (const auto& item : calendars)
        {
            retained.insert(item.id);
            upsert.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            upsert.bindValue(QStringLiteral(":id"), QString::fromStdString(item.id));
            upsert.bindValue(QStringLiteral(":name"), QString::fromStdString(item.name));
            upsert.bindValue(QStringLiteral(":description"), optionalString(item.description));
            upsert.bindValue(QStringLiteral(":color"), optionalString(item.color));
            upsert.bindValue(QStringLiteral(":sort"), item.sortOrder);
            upsert.bindValue(QStringLiteral(":subscribed"), item.isSubscribed ? 1 : 0);
            upsert.bindValue(QStringLiteral(":visible"), item.isVisible ? 1 : 0);
            upsert.bindValue(QStringLiteral(":default"), item.isDefault ? 1 : 0);
            upsert.bindValue(QStringLiteral(":time_zone"),
                             item.timeZone ? QVariant{QString::fromStdString(item.timeZone->value)}
                                           : QVariant{});
            upsert.bindValue(QStringLiteral(":rights"), rightsMask(item.myRights));
            upsert.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
            if (!exec(upsert, failure, QStringLiteral("Upsert calendar")))
                break;
        }
        if (!failure)
        {
            QSqlQuery existing{database};
            existing.prepare(
                QStringLiteral("SELECT calendar_id FROM calendars WHERE account_id=:account"));
            existing.bindValue(QStringLiteral(":account"),
                               QString::fromStdString(std::string{accountId}));
            if (!exec(existing, failure, QStringLiteral("List stale calendars")))
            {
                database.rollback();
                return failure;
            }
            std::vector<QString> stale;
            while (existing.next())
            {
                if (!retained.contains(existing.value(0).toString().toStdString()))
                    stale.push_back(existing.value(0).toString());
            }
            QSqlQuery remove{database};
            remove.prepare(QStringLiteral(
                "DELETE FROM calendars WHERE account_id=:account AND calendar_id=:id"));
            for (const auto& id : stale)
            {
                remove.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(std::string{accountId}));
                remove.bindValue(QStringLiteral(":id"), id);
                if (!exec(remove, failure, QStringLiteral("Remove stale calendar")))
                    break;
            }
        }
        if (failure)
        {
            database.rollback();
            return failure;
        }
        QSqlQuery stateToken{database};
        stateToken.prepare(QStringLiteral(
            "INSERT INTO calendar_state_tokens (account_id,data_type,state) VALUES "
            "(:account,'Calendar',:state) ON CONFLICT(account_id,data_type) DO UPDATE SET "
            "state=excluded.state"));
        stateToken.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
        stateToken.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!stateToken.exec())
        {
            database.rollback();
            return queryError(QStringLiteral("Store Calendar state"), stateToken);
        }
        if (!database.commit())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit calendar replacement: ") +
                                            database.lastError().text()};
        return std::nullopt;
    }

    std::variant<std::vector<calendar::Calendar>, DatabaseError>
    CalendarRepository::listCalendars(const std::string_view accountId) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT calendar_id,name,description,color,sort_order,is_subscribed,is_visible,"
            "is_default,time_zone,rights_json FROM calendars WHERE account_id=:account ORDER BY "
            "sort_order,name COLLATE NOCASE,calendar_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        if (!query.exec())
            return queryError(QStringLiteral("List calendars"), query);
        std::vector<calendar::Calendar> result;
        while (query.next())
        {
            result.push_back(calendar::Calendar{
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
                .myRights = rights(query.value(9).toUInt())});
        }
        return result;
    }

    std::variant<std::vector<CalendarAccount>, DatabaseError>
    CalendarRepository::listAccounts() const
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
    CalendarRepository::stateToken(const std::string_view accountId,
                                   const std::string_view dataType) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        if (dataType != "Calendar" && dataType != "CalendarEvent")
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Unsupported calendar state type")};
        QSqlQuery query{m_connection.database()};
        query.prepare(
            QStringLiteral("SELECT state FROM calendar_state_tokens WHERE account_id=:account AND "
                           "data_type=:type"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":type"), QString::fromStdString(std::string{dataType}));
        if (!query.exec())
            return queryError(QStringLiteral("Load calendar state"), query);
        if (!query.next())
            return std::optional<std::string>{std::nullopt};
        return std::optional{query.value(0).toString().toStdString()};
    }

    std::optional<DatabaseError> CalendarRepository::reconcileWindow(const CalendarWindow& window)
    {
        if (const auto error = m_connection.validate())
            return error;
        auto& database = m_connection.database();
        if (!database.transaction())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin calendar reconciliation: ") +
                                            database.lastError().text()};
        std::optional<DatabaseError> failure;
        QSqlQuery upsertEvent{database};
        upsertEvent.prepare(QStringLiteral(
            "INSERT INTO calendar_events (account_id,event_id,uid,title,description,location,"
            "document_json,state) VALUES (:account,:id,:uid,:title,:description,:location,"
            ":document,:state) ON CONFLICT(account_id,event_id) DO UPDATE SET uid=excluded.uid,"
            "title=excluded.title,description=excluded.description,location=excluded.location,"
            "document_json=excluded.document_json,state=excluded.state"));
        QSqlQuery clearMembership{database};
        clearMembership.prepare(QStringLiteral(
            "DELETE FROM calendar_event_calendars WHERE account_id=:account AND event_id=:id"));
        QSqlQuery addMembership{database};
        addMembership.prepare(QStringLiteral(
            "INSERT INTO calendar_event_calendars (account_id,event_id,calendar_id) VALUES "
            "(:account,:event,:calendar)"));
        for (const auto& item : window.events)
        {
            const auto document = api::serializeCalendarEventDocument(item);
            if (!document)
            {
                failure = DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                        .message = QStringLiteral("Serialize calendar event")};
                break;
            }
            upsertEvent.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(window.accountId));
            upsertEvent.bindValue(QStringLiteral(":id"), QString::fromStdString(item.id));
            upsertEvent.bindValue(QStringLiteral(":uid"), QString::fromStdString(item.uid));
            upsertEvent.bindValue(QStringLiteral(":title"), QString::fromStdString(item.title));
            upsertEvent.bindValue(QStringLiteral(":description"), optionalString(item.description));
            upsertEvent.bindValue(QStringLiteral(":location"), optionalString(item.location));
            upsertEvent.bindValue(QStringLiteral(":document"), QString::fromStdString(*document));
            upsertEvent.bindValue(QStringLiteral(":state"),
                                  QString::fromStdString(window.eventState));
            if (!exec(upsertEvent, failure, QStringLiteral("Upsert calendar event")))
                break;
            clearMembership.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(window.accountId));
            clearMembership.bindValue(QStringLiteral(":id"), QString::fromStdString(item.id));
            if (!exec(clearMembership, failure, QStringLiteral("Clear event calendars")))
                break;
            for (const auto& [calendarId, present] : item.calendarIds)
            {
                if (!present)
                    continue;
                addMembership.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(window.accountId));
                addMembership.bindValue(QStringLiteral(":event"), QString::fromStdString(item.id));
                addMembership.bindValue(QStringLiteral(":calendar"),
                                        QString::fromStdString(calendarId));
                if (!exec(addMembership, failure, QStringLiteral("Add event calendar")))
                    break;
            }
            if (failure)
                break;
        }
        QSqlQuery upsertOccurrence{database};
        upsertOccurrence.prepare(QStringLiteral(
            "INSERT INTO calendar_occurrences (account_id,occurrence_id,event_id,recurrence_id,"
            "start_utc,end_utc,local_start,local_end,is_all_day) VALUES "
            "(:account,:id,:event,:recurrence,:start_utc,:end_utc,:local_start,:local_end,:all_day)"
            " "
            "ON CONFLICT(account_id,occurrence_id) DO UPDATE SET event_id=excluded.event_id,"
            "recurrence_id=excluded.recurrence_id,start_utc=excluded.start_utc,"
            "end_utc=excluded.end_utc,local_start=excluded.local_start,local_end=excluded.local_"
            "end,"
            "is_all_day=excluded.is_all_day"));
        if (!failure)
        {
            for (const auto& item : window.occurrences)
            {
                upsertOccurrence.bindValue(QStringLiteral(":account"),
                                           QString::fromStdString(window.accountId));
                upsertOccurrence.bindValue(QStringLiteral(":id"), QString::fromStdString(item.id));
                upsertOccurrence.bindValue(QStringLiteral(":event"),
                                           QString::fromStdString(item.eventId));
                upsertOccurrence.bindValue(
                    QStringLiteral(":recurrence"),
                    item.recurrenceId ? QVariant{QString::fromStdString(item.recurrenceId->value)}
                                      : QVariant{});
                upsertOccurrence.bindValue(
                    QStringLiteral(":start_utc"),
                    item.utcStart ? QVariant{QString::fromStdString(item.utcStart->value)}
                                  : QVariant{});
                upsertOccurrence.bindValue(
                    QStringLiteral(":end_utc"),
                    item.utcEnd ? QVariant{QString::fromStdString(item.utcEnd->value)}
                                : QVariant{});
                upsertOccurrence.bindValue(QStringLiteral(":local_start"),
                                           QString::fromStdString(item.localStart.value));
                upsertOccurrence.bindValue(QStringLiteral(":local_end"),
                                           QString::fromStdString(item.localEnd.value));
                upsertOccurrence.bindValue(QStringLiteral(":all_day"), item.allDay ? 1 : 0);
                if (!exec(upsertOccurrence, failure, QStringLiteral("Upsert calendar occurrence")))
                    break;
            }
        }
        const auto bindWindow = [&](QSqlQuery& query)
        {
            query.bindValue(QStringLiteral(":account"), QString::fromStdString(window.accountId));
            query.bindValue(QStringLiteral(":start"), QString::fromStdString(window.start.value));
            query.bindValue(QStringLiteral(":end"), QString::fromStdString(window.end.value));
            query.bindValue(QStringLiteral(":zone"),
                            QString::fromStdString(window.displayTimeZone.value));
        };
        if (!failure)
        {
            QSqlQuery stateToken{database};
            stateToken.prepare(QStringLiteral(
                "INSERT INTO calendar_state_tokens (account_id,data_type,state) VALUES "
                "(:account,'CalendarEvent',:state) ON CONFLICT(account_id,data_type) DO UPDATE SET "
                "state=excluded.state"));
            stateToken.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(window.accountId));
            stateToken.bindValue(QStringLiteral(":state"),
                                 QString::fromStdString(window.eventState));
            exec(stateToken, failure, QStringLiteral("Store CalendarEvent state"));
        }
        if (!failure)
        {
            QSqlQuery upsertWindow{database};
            upsertWindow.prepare(QStringLiteral(
                "INSERT INTO calendar_query_windows (account_id,range_start,range_end,"
                "display_time_zone,query_state) VALUES (:account,:start,:end,:zone,:state) ON "
                "CONFLICT(account_id,range_start,range_end,display_time_zone) DO UPDATE SET "
                "query_state=excluded.query_state,updated_at=CURRENT_TIMESTAMP"));
            bindWindow(upsertWindow);
            upsertWindow.bindValue(QStringLiteral(":state"),
                                   QString::fromStdString(window.queryState));
            exec(upsertWindow, failure, QStringLiteral("Upsert calendar window"));
        }
        if (!failure)
        {
            QSqlQuery clear{database};
            clear.prepare(QStringLiteral(
                "DELETE FROM calendar_window_occurrences WHERE account_id=:account AND "
                "range_start=:start AND range_end=:end AND display_time_zone=:zone"));
            bindWindow(clear);
            exec(clear, failure, QStringLiteral("Clear calendar window membership"));
        }
        if (!failure)
        {
            QSqlQuery add{database};
            add.prepare(QStringLiteral(
                "INSERT INTO calendar_window_occurrences (account_id,range_start,range_end,"
                "display_time_zone,occurrence_id) VALUES (:account,:start,:end,:zone,:id)"));
            for (const auto& item : window.occurrences)
            {
                bindWindow(add);
                add.bindValue(QStringLiteral(":id"), QString::fromStdString(item.id));
                if (!exec(add, failure, QStringLiteral("Add calendar window membership")))
                    break;
            }
        }
        if (!failure)
        {
            QSqlQuery pruneOccurrences{database};
            pruneOccurrences.prepare(QStringLiteral(
                "DELETE FROM calendar_occurrences WHERE account_id=:account AND NOT EXISTS "
                "(SELECT 1 FROM calendar_window_occurrences w WHERE w.account_id="
                "calendar_occurrences.account_id AND w.occurrence_id="
                "calendar_occurrences.occurrence_id)"));
            pruneOccurrences.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(window.accountId));
            exec(pruneOccurrences, failure, QStringLiteral("Prune unreferenced occurrences"));
        }
        if (!failure)
        {
            QSqlQuery pruneEvents{database};
            pruneEvents.prepare(QStringLiteral(
                "DELETE FROM calendar_events WHERE account_id=:account AND NOT EXISTS "
                "(SELECT 1 FROM calendar_occurrences o WHERE "
                "o.account_id=calendar_events.account_id "
                "AND o.event_id=calendar_events.event_id)"));
            pruneEvents.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(window.accountId));
            exec(pruneEvents, failure, QStringLiteral("Prune unreferenced calendar events"));
        }
        if (failure)
        {
            database.rollback();
            return failure;
        }
        if (!database.commit())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit calendar reconciliation: ") +
                                            database.lastError().text()};
        return std::nullopt;
    }

    std::variant<std::optional<CalendarWindow>, DatabaseError> CalendarRepository::loadWindow(
        const std::string_view accountId, const calendar::LocalDateTime& start,
        const calendar::LocalDateTime& end, const calendar::TimeZoneId& displayTimeZone) const
    {
        if (const auto error = m_connection.validate())
            return *error;
        QSqlQuery windowQuery{m_connection.database()};
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
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT o.occurrence_id,o.event_id,o.recurrence_id,o.start_utc,o.end_utc,o.local_start,"
            "o.local_end,o.is_all_day,e.document_json FROM calendar_window_occurrences w JOIN "
            "calendar_occurrences o ON o.account_id=w.account_id AND "
            "o.occurrence_id=w.occurrence_id "
            "JOIN calendar_events e ON e.account_id=o.account_id AND e.event_id=o.event_id WHERE "
            "w.account_id=:account AND w.range_start=:start AND w.range_end=:end AND "
            "w.display_time_zone=:zone ORDER BY o.local_start,o.local_end,o.occurrence_id"));
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
                if (!parsed.ok())
                    return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                         .message = QStringLiteral("Parse cached calendar event")};
                result.events.push_back(*parsed.value);
            }
        }
        return std::optional<CalendarWindow>{std::move(result)};
    }
} // namespace javelin::jmap::cache
