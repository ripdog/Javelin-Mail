#include "jmap/cache/CalendarRepository.h"

#include "jmap/api/CalendarMethods.h"
#include "jmap/calendar/CalendarEventEditing.h"

#include <QSqlError>
#include <QSqlQuery>

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace javelin::jmap::cache
{
    namespace
    {
        constexpr int maxCachedWindowsPerAccountAndTimeZone = 12;

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
        const DatabaseWriteScope writeScope{m_connection};
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
        QSqlQuery removeAlerts{database};
        removeAlerts.prepare(
            QStringLiteral("DELETE FROM calendar_default_alerts WHERE account_id=:account AND "
                           "calendar_id=:calendar"));
        QSqlQuery insertAlert{database};
        insertAlert.prepare(QStringLiteral(
            "INSERT INTO calendar_default_alerts(account_id,calendar_id,alert_id,without_time,"
            "action,trigger_kind,relative_to,offset,trigger_at,acknowledged) VALUES "
            "(:account,:calendar,:alert,:without_time,:action,:kind,:relative,:offset,:when,"
            ":acknowledged)"));
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
            removeAlerts.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(std::string{accountId}));
            removeAlerts.bindValue(QStringLiteral(":calendar"), QString::fromStdString(item.id));
            if (!exec(removeAlerts, failure, QStringLiteral("Replace calendar default alerts")))
                break;
            const auto writeAlerts = [&](const auto& alerts, const bool withoutTime)
            {
                for (const auto& [alertId, alert] : alerts)
                {
                    insertAlert.bindValue(QStringLiteral(":account"),
                                          QString::fromStdString(std::string{accountId}));
                    insertAlert.bindValue(QStringLiteral(":calendar"),
                                          QString::fromStdString(item.id));
                    insertAlert.bindValue(QStringLiteral(":alert"),
                                          QString::fromStdString(alertId));
                    insertAlert.bindValue(QStringLiteral(":without_time"), withoutTime ? 1 : 0);
                    insertAlert.bindValue(QStringLiteral(":action"),
                                          QString::fromStdString(alert.action));
                    insertAlert.bindValue(QStringLiteral(":kind"),
                                          alert.triggerKind == calendar::AlertTriggerKind::Absolute
                                              ? QStringLiteral("absolute")
                                              : QStringLiteral("offset"));
                    insertAlert.bindValue(QStringLiteral(":relative"),
                                          QString::fromStdString(alert.relativeTo));
                    insertAlert.bindValue(
                        QStringLiteral(":offset"),
                        alert.offset ? QVariant{QString::fromStdString(alert.offset->value)}
                                     : QVariant{});
                    insertAlert.bindValue(QStringLiteral(":when"),
                                          alert.when
                                              ? QVariant{QString::fromStdString(alert.when->value)}
                                              : QVariant{});
                    insertAlert.bindValue(QStringLiteral(":acknowledged"),
                                          alert.acknowledged ? QVariant{QString::fromStdString(
                                                                   alert.acknowledged->value)}
                                                             : QVariant{});
                    if (!exec(insertAlert, failure, QStringLiteral("Store calendar default alert")))
                        return false;
                }
                return true;
            };
            if (!writeAlerts(item.defaultAlertsWithTime, false) ||
                !writeAlerts(item.defaultAlertsWithoutTime, true))
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
            "SELECT c.calendar_id,c.name,c.description,c.color,c.sort_order,c.is_subscribed,"
            "COALESCE(p.is_visible,c.is_visible),c.is_default,c.time_zone,c.rights_json FROM "
            "calendars c LEFT JOIN calendar_preferences p ON p.account_id=c.account_id AND "
            "p.calendar_id=c.calendar_id WHERE c.account_id=:account AND NOT EXISTS (SELECT 1 "
            "FROM calendar_deletion_projections d WHERE d.account_id=c.account_id AND "
            "d.calendar_id=c.calendar_id) ORDER BY c.sort_order,"
            "c.name COLLATE NOCASE,c.calendar_id"));
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
            QSqlQuery alerts{m_connection.database()};
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

    std::optional<DatabaseError>
    CalendarRepository::setCalendarVisible(const std::string_view accountId,
                                           const std::string_view calendarId, const bool visible)
    {
        if (const auto error = m_connection.validate())
            return error;
        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO calendar_preferences (account_id,calendar_id,is_visible) VALUES "
            "(:account,:calendar,:visible) ON CONFLICT(account_id,calendar_id) DO UPDATE SET "
            "is_visible=excluded.is_visible"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":calendar"),
                        QString::fromStdString(std::string{calendarId}));
        query.bindValue(QStringLiteral(":visible"), visible ? 1 : 0);
        if (!query.exec())
            return queryError(QStringLiteral("Store calendar visibility"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::applyCalendarSubscription(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view calendarId, const std::string_view state, const bool subscribed)
    {
        if (!transaction.isActive())
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Apply calendar subscription without transaction")};
        auto& database = transaction.connection().database();
        QSqlQuery update{database};
        update.prepare(
            QStringLiteral("UPDATE calendars SET is_subscribed=:subscribed,state=:state WHERE "
                           "account_id=:account AND calendar_id=:calendar"));
        update.bindValue(QStringLiteral(":subscribed"), subscribed ? 1 : 0);
        update.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        update.bindValue(QStringLiteral(":account"),
                         QString::fromStdString(std::string{accountId}));
        update.bindValue(QStringLiteral(":calendar"),
                         QString::fromStdString(std::string{calendarId}));
        if (!update.exec())
            return queryError(QStringLiteral("Apply server calendar subscription"), update);
        if (update.numRowsAffected() != 1)
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Calendar subscription target missing")};

        QSqlQuery stateToken{database};
        stateToken.prepare(QStringLiteral(
            "INSERT INTO calendar_state_tokens (account_id,data_type,state) VALUES "
            "(:account,'Calendar',:state) ON CONFLICT(account_id,data_type) DO UPDATE SET "
            "state=excluded.state"));
        stateToken.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
        stateToken.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!stateToken.exec())
            return queryError(QStringLiteral("Store Calendar state after subscription change"),
                              stateToken);
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::applyCalendarDefaults(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view state, const std::unordered_map<std::string, bool>& defaults)
    {
        if (!transaction.isActive())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Apply defaults without transaction")};
        auto& database = transaction.connection().database();
        if (std::ranges::any_of(defaults, [](const auto& item) { return item.second; }))
        {
            QSqlQuery clear{database};
            clear.prepare(QStringLiteral(
                "UPDATE calendars SET is_default=0,state=:state WHERE account_id=:account"));
            clear.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
            clear.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
            if (!clear.exec())
                return queryError(QStringLiteral("Clear previous server calendar default"), clear);
        }
        QSqlQuery update{database};
        update.prepare(QStringLiteral(
            "UPDATE calendars SET is_default=:default,state=:state WHERE account_id=:account AND "
            "calendar_id=:calendar"));
        for (const auto& [calendarId, isDefault] : defaults)
        {
            update.bindValue(QStringLiteral(":default"), isDefault ? 1 : 0);
            update.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
            update.bindValue(QStringLiteral(":account"),
                             QString::fromStdString(std::string{accountId}));
            update.bindValue(QStringLiteral(":calendar"), QString::fromStdString(calendarId));
            if (!update.exec())
                return queryError(QStringLiteral("Apply server calendar default"), update);
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
            return queryError(QStringLiteral("Store Calendar state after default change"),
                              stateToken);
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::projectCalendarCreation(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view state, const calendar::Calendar& calendar)
    {
        if (!transaction.isActive())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Project calendar without transaction")};
        QSqlQuery query{transaction.connection().database()};
        query.prepare(QStringLiteral(
            "INSERT INTO calendars (account_id,calendar_id,name,description,color,sort_order,"
            "is_subscribed,is_visible,is_default,time_zone,rights_json,state) VALUES "
            "(:account,:id,:name,:description,:color,:sort,:subscribed,:visible,:default,"
            ":time_zone,:rights,:state)"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(calendar.id));
        query.bindValue(QStringLiteral(":name"), QString::fromStdString(calendar.name));
        query.bindValue(QStringLiteral(":description"), optionalString(calendar.description));
        query.bindValue(QStringLiteral(":color"), optionalString(calendar.color));
        query.bindValue(QStringLiteral(":sort"), calendar.sortOrder);
        query.bindValue(QStringLiteral(":subscribed"), calendar.isSubscribed ? 1 : 0);
        query.bindValue(QStringLiteral(":visible"), calendar.isVisible ? 1 : 0);
        query.bindValue(QStringLiteral(":default"), calendar.isDefault ? 1 : 0);
        query.bindValue(QStringLiteral(":time_zone"),
                        calendar.timeZone
                            ? QVariant{QString::fromStdString(calendar.timeZone->value)}
                            : QVariant{});
        query.bindValue(QStringLiteral(":rights"), rightsMask(calendar.myRights));
        query.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!query.exec())
            return queryError(QStringLiteral("Project calendar creation"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::projectCalendarDeletion(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view calendarId, const std::string_view mutationId)
    {
        if (!transaction.isActive())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Project deletion without transaction")};
        QSqlQuery query{transaction.connection().database()};
        query.prepare(QStringLiteral(
            "INSERT INTO calendar_deletion_projections(account_id,calendar_id,mutation_id) "
            "VALUES(:account,:calendar,:mutation)"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":calendar"),
                        QString::fromStdString(std::string{calendarId}));
        query.bindValue(QStringLiteral(":mutation"),
                        QString::fromStdString(std::string{mutationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Project calendar deletion"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    CalendarRepository::clearCalendarDeletion(DatabaseTransaction& transaction,
                                              const std::string_view mutationId)
    {
        QSqlQuery query{transaction.connection().database()};
        query.prepare(
            QStringLiteral("DELETE FROM calendar_deletion_projections WHERE mutation_id=:id"));
        query.bindValue(QStringLiteral(":id"), QString::fromStdString(std::string{mutationId}));
        if (!query.exec())
            return queryError(QStringLiteral("Clear calendar deletion projection"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError>
    CalendarRepository::removeProjectedCalendar(DatabaseTransaction& transaction,
                                                const std::string_view accountId,
                                                const std::string_view calendarId)
    {
        QSqlQuery query{transaction.connection().database()};
        query.prepare(QStringLiteral(
            "DELETE FROM calendars WHERE account_id=:account AND calendar_id=:calendar"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":calendar"),
                        QString::fromStdString(std::string{calendarId}));
        if (!query.exec())
            return queryError(QStringLiteral("Remove projected calendar"), query);
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::acceptProjectedCalendar(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view projectedId, const std::string_view acceptedId,
        const std::string_view state, const bool isDefault)
    {
        auto& database = transaction.connection().database();
        if (isDefault)
        {
            QSqlQuery clear{database};
            clear.prepare(
                QStringLiteral("UPDATE calendars SET is_default=0 WHERE account_id=:account"));
            clear.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
            if (!clear.exec())
                return queryError(QStringLiteral("Clear calendar default for creation"), clear);
        }
        QSqlQuery update{database};
        update.prepare(QStringLiteral(
            "UPDATE calendars SET calendar_id=:accepted,is_default=:default,state=:state "
            "WHERE account_id=:account AND calendar_id=:projected"));
        update.bindValue(QStringLiteral(":accepted"),
                         QString::fromStdString(std::string{acceptedId}));
        update.bindValue(QStringLiteral(":default"), isDefault ? 1 : 0);
        update.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        update.bindValue(QStringLiteral(":account"),
                         QString::fromStdString(std::string{accountId}));
        update.bindValue(QStringLiteral(":projected"),
                         QString::fromStdString(std::string{projectedId}));
        if (!update.exec())
            return queryError(QStringLiteral("Accept projected calendar"), update);
        QSqlQuery token{database};
        token.prepare(QStringLiteral(
            "INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
            "(:account,'Calendar',:state) ON CONFLICT(account_id,data_type) DO UPDATE SET "
            "state=excluded.state"));
        token.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        token.bindValue(QStringLiteral(":state"), QString::fromStdString(std::string{state}));
        if (!token.exec())
            return queryError(QStringLiteral("Store Calendar state after creation"), token);
        return std::nullopt;
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
        if (dataType != "Calendar" && dataType != "CalendarEvent" &&
            dataType != "CalendarEventNotification")
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

    std::variant<std::optional<calendar::CalendarEvent>, DatabaseError>
    CalendarRepository::findEvent(const std::string_view accountId,
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
            return std::optional<calendar::CalendarEvent>{};
        const auto parsed =
            api::parseCalendarEventDocument(accountId, query.value(0).toString().toStdString());
        if (!parsed.ok() || !parsed.value.has_value())
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Cached calendar event document is invalid."),
            };
        }
        return std::optional<calendar::CalendarEvent>{std::move(*parsed.value)};
    }

    std::optional<DatabaseError>
    CalendarRepository::storeStateTokens(const std::string_view accountId,
                                         const std::string_view calendarState,
                                         const std::string_view eventState)
    {
        if (const auto error = m_connection.validate())
            return error;
        const DatabaseWriteScope writeScope{m_connection};
        auto& database = m_connection.database();
        if (!database.transaction())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin calendar state update: ") +
                                            database.lastError().text()};
        QSqlQuery query{database};
        query.prepare(QStringLiteral(
            "INSERT INTO calendar_state_tokens (account_id,data_type,state) VALUES "
            "(:account,:type,:state) ON CONFLICT(account_id,data_type) DO UPDATE SET "
            "state=excluded.state"));
        for (const auto& [type, state] :
             {std::pair{QStringLiteral("Calendar"),
                        QString::fromStdString(std::string{calendarState})},
              std::pair{QStringLiteral("CalendarEvent"),
                        QString::fromStdString(std::string{eventState})}})
        {
            query.bindValue(QStringLiteral(":account"),
                            QString::fromStdString(std::string{accountId}));
            query.bindValue(QStringLiteral(":type"), type);
            query.bindValue(QStringLiteral(":state"), state);
            if (!query.exec())
            {
                database.rollback();
                return queryError(QStringLiteral("Store calendar states"), query);
            }
        }
        if (!database.commit())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit calendar state update: ") +
                                            database.lastError().text()};
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::reconcileWindow(const CalendarWindow& window)
    {
        if (const auto error = m_connection.validate())
            return error;
        const DatabaseWriteScope writeScope{m_connection};
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
                "display_time_zone,query_state,updated_at) VALUES "
                "(:account,:start,:end,:zone,:state,strftime('%Y-%m-%dT%H:%M:%fZ','now')) ON "
                "CONFLICT(account_id,range_start,range_end,display_time_zone) DO UPDATE SET "
                "query_state=excluded.query_state,updated_at=excluded.updated_at"));
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
            QSqlQuery pruneWindows{database};
            pruneWindows.prepare(QStringLiteral(
                "DELETE FROM calendar_query_windows WHERE rowid IN (SELECT rowid FROM "
                "calendar_query_windows WHERE account_id=:account AND display_time_zone=:zone "
                "ORDER BY updated_at DESC,rowid DESC LIMIT -1 OFFSET :retained)"));
            pruneWindows.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(window.accountId));
            pruneWindows.bindValue(QStringLiteral(":zone"),
                                   QString::fromStdString(window.displayTimeZone.value));
            pruneWindows.bindValue(QStringLiteral(":retained"),
                                   maxCachedWindowsPerAccountAndTimeZone);
            exec(pruneWindows, failure, QStringLiteral("Prune old calendar windows"));
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
                "AND o.event_id=calendar_events.event_id) AND NOT EXISTS (SELECT 1 FROM "
                "calendar_pending_invitations p WHERE p.account_id=calendar_events.account_id "
                "AND p.event_id=calendar_events.event_id)"));
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

    std::optional<DatabaseError> CalendarRepository::applyEventDelta(
        const std::string_view accountId, const std::string_view calendarState,
        const std::string_view eventState, const calendar::TimeZoneId& displayTimeZone,
        const std::vector<calendar::CalendarEvent>& events,
        const std::vector<calendar::Occurrence>& occurrences,
        const std::vector<std::string>& destroyedEventIds)
    {
        if (const auto error = m_connection.validate())
            return error;
        const DatabaseWriteScope writeScope{m_connection};
        auto& database = m_connection.database();
        if (!database.transaction())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Begin calendar delta: ") +
                                            database.lastError().text()};
        std::optional<DatabaseError> failure;
        QSqlQuery removeOccurrences{database};
        removeOccurrences.prepare(QStringLiteral(
            "DELETE FROM calendar_occurrences WHERE account_id=:account AND event_id=:event"));
        const auto removeEventOccurrences = [&](const std::string_view eventId)
        {
            removeOccurrences.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(std::string{accountId}));
            removeOccurrences.bindValue(QStringLiteral(":event"),
                                        QString::fromStdString(std::string{eventId}));
            return exec(removeOccurrences, failure, QStringLiteral("Remove event occurrences"));
        };
        for (const auto& event : events)
            if (!removeEventOccurrences(event.id))
                break;
        for (const auto& eventId : destroyedEventIds)
            if (!failure && !removeEventOccurrences(eventId))
                break;

        QSqlQuery removeEvent{database};
        removeEvent.prepare(QStringLiteral(
            "DELETE FROM calendar_events WHERE account_id=:account AND event_id=:event"));
        for (const auto& eventId : destroyedEventIds)
        {
            if (failure)
                break;
            removeEvent.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(std::string{accountId}));
            removeEvent.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
            exec(removeEvent, failure, QStringLiteral("Remove calendar event"));
        }

        QSqlQuery upsertEvent{database};
        upsertEvent.prepare(QStringLiteral(
            "INSERT INTO calendar_events (account_id,event_id,uid,title,description,location,"
            "document_json,state) VALUES (:account,:id,:uid,:title,:description,:location,"
            ":document,:state) ON CONFLICT(account_id,event_id) DO UPDATE SET uid=excluded.uid,"
            "title=excluded.title,description=excluded.description,location=excluded.location,"
            "document_json=excluded.document_json,state=excluded.state"));
        QSqlQuery clearMembership{database};
        clearMembership.prepare(QStringLiteral(
            "DELETE FROM calendar_event_calendars WHERE account_id=:account AND event_id=:event"));
        QSqlQuery addMembership{database};
        addMembership.prepare(QStringLiteral(
            "INSERT INTO calendar_event_calendars (account_id,event_id,calendar_id) VALUES "
            "(:account,:event,:calendar)"));
        for (const auto& event : events)
        {
            if (failure)
                break;
            const auto document = api::serializeCalendarEventDocument(event);
            if (!document)
            {
                failure = DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                        .message = QStringLiteral("Serialize calendar event")};
                break;
            }
            upsertEvent.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(std::string{accountId}));
            upsertEvent.bindValue(QStringLiteral(":id"), QString::fromStdString(event.id));
            upsertEvent.bindValue(QStringLiteral(":uid"), QString::fromStdString(event.uid));
            upsertEvent.bindValue(QStringLiteral(":title"), QString::fromStdString(event.title));
            upsertEvent.bindValue(QStringLiteral(":description"),
                                  optionalString(event.description));
            upsertEvent.bindValue(QStringLiteral(":location"), optionalString(event.location));
            upsertEvent.bindValue(QStringLiteral(":document"), QString::fromStdString(*document));
            upsertEvent.bindValue(QStringLiteral(":state"),
                                  QString::fromStdString(std::string{eventState}));
            if (!exec(upsertEvent, failure, QStringLiteral("Upsert changed calendar event")))
                break;
            clearMembership.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(std::string{accountId}));
            clearMembership.bindValue(QStringLiteral(":event"), QString::fromStdString(event.id));
            if (!exec(clearMembership, failure, QStringLiteral("Clear changed event calendars")))
                break;
            for (const auto& [calendarId, present] : event.calendarIds)
            {
                if (!present)
                    continue;
                addMembership.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(std::string{accountId}));
                addMembership.bindValue(QStringLiteral(":event"), QString::fromStdString(event.id));
                addMembership.bindValue(QStringLiteral(":calendar"),
                                        QString::fromStdString(calendarId));
                if (!exec(addMembership, failure, QStringLiteral("Add changed event calendar")))
                    break;
            }
        }

        QSqlQuery upsertOccurrence{database};
        upsertOccurrence.prepare(QStringLiteral(
            "INSERT INTO calendar_occurrences (account_id,occurrence_id,event_id,recurrence_id,"
            "start_utc,end_utc,local_start,local_end,is_all_day) VALUES "
            "(:account,:id,:event,:recurrence,:start_utc,:end_utc,:local_start,:local_end,:all_"
            "day)"));
        QSqlQuery addToWindows{database};
        addToWindows.prepare(QStringLiteral(
            "INSERT INTO calendar_window_occurrences (account_id,range_start,range_end,"
            "display_time_zone,occurrence_id) SELECT account_id,range_start,range_end,"
            "display_time_zone,:occurrence FROM calendar_query_windows WHERE account_id=:account "
            "AND display_time_zone=:zone AND range_start < :local_end AND range_end > "
            ":local_start"));
        for (const auto& occurrence : occurrences)
        {
            if (failure)
                break;
            upsertOccurrence.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(std::string{accountId}));
            upsertOccurrence.bindValue(QStringLiteral(":id"),
                                       QString::fromStdString(occurrence.id));
            upsertOccurrence.bindValue(QStringLiteral(":event"),
                                       QString::fromStdString(occurrence.eventId));
            upsertOccurrence.bindValue(
                QStringLiteral(":recurrence"),
                occurrence.recurrenceId
                    ? QVariant{QString::fromStdString(occurrence.recurrenceId->value)}
                    : QVariant{});
            upsertOccurrence.bindValue(
                QStringLiteral(":start_utc"),
                occurrence.utcStart ? QVariant{QString::fromStdString(occurrence.utcStart->value)}
                                    : QVariant{});
            upsertOccurrence.bindValue(
                QStringLiteral(":end_utc"),
                occurrence.utcEnd ? QVariant{QString::fromStdString(occurrence.utcEnd->value)}
                                  : QVariant{});
            upsertOccurrence.bindValue(QStringLiteral(":local_start"),
                                       QString::fromStdString(occurrence.localStart.value));
            upsertOccurrence.bindValue(QStringLiteral(":local_end"),
                                       QString::fromStdString(occurrence.localEnd.value));
            upsertOccurrence.bindValue(QStringLiteral(":all_day"), occurrence.allDay ? 1 : 0);
            if (!exec(upsertOccurrence, failure, QStringLiteral("Upsert changed occurrence")))
                break;
            addToWindows.bindValue(QStringLiteral(":occurrence"),
                                   QString::fromStdString(occurrence.id));
            addToWindows.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(std::string{accountId}));
            addToWindows.bindValue(QStringLiteral(":zone"),
                                   QString::fromStdString(displayTimeZone.value));
            addToWindows.bindValue(QStringLiteral(":local_start"),
                                   QString::fromStdString(occurrence.localStart.value));
            addToWindows.bindValue(QStringLiteral(":local_end"),
                                   QString::fromStdString(occurrence.localEnd.value));
            exec(addToWindows, failure, QStringLiteral("Add occurrence to cached windows"));
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
                                       QString::fromStdString(std::string{accountId}));
            exec(pruneOccurrences, failure, QStringLiteral("Prune delta occurrences"));
        }
        if (!failure)
        {
            QSqlQuery pruneEvents{database};
            pruneEvents.prepare(QStringLiteral(
                "DELETE FROM calendar_events WHERE account_id=:account AND NOT EXISTS "
                "(SELECT 1 FROM calendar_occurrences o WHERE o.account_id="
                "calendar_events.account_id AND o.event_id=calendar_events.event_id) AND NOT "
                "EXISTS (SELECT 1 FROM calendar_pending_invitations p WHERE p.account_id="
                "calendar_events.account_id AND p.event_id=calendar_events.event_id)"));
            pruneEvents.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(std::string{accountId}));
            exec(pruneEvents, failure, QStringLiteral("Prune delta events"));
        }
        if (!failure)
        {
            QSqlQuery states{database};
            states.prepare(QStringLiteral(
                "INSERT INTO calendar_state_tokens (account_id,data_type,state) VALUES "
                "(:account,:type,:state) ON CONFLICT(account_id,data_type) DO UPDATE SET "
                "state=excluded.state"));
            for (const auto& [type, state] :
                 {std::pair{QStringLiteral("Calendar"),
                            QString::fromStdString(std::string{calendarState})},
                  std::pair{QStringLiteral("CalendarEvent"),
                            QString::fromStdString(std::string{eventState})}})
            {
                states.bindValue(QStringLiteral(":account"),
                                 QString::fromStdString(std::string{accountId}));
                states.bindValue(QStringLiteral(":type"), type);
                states.bindValue(QStringLiteral(":state"), state);
                if (!exec(states, failure, QStringLiteral("Store delta states")))
                    break;
            }
        }
        if (failure)
        {
            database.rollback();
            return failure;
        }
        if (!database.commit())
            return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                 .message = QStringLiteral("Commit calendar delta: ") +
                                            database.lastError().text()};
        return std::nullopt;
    }

    std::optional<DatabaseError> CalendarRepository::projectEvents(
        DatabaseTransaction& transaction, const std::string_view accountId,
        const std::string_view eventState, const std::vector<calendar::CalendarEvent>& events,
        const std::vector<calendar::Occurrence>& nonRecurringOccurrences,
        const std::span<const std::string> destroyedEventIds)
    {
        if (const auto error = m_connection.validate())
            return error;
        if (!transaction.isActive() || &transaction.connection() != &m_connection)
        {
            return DatabaseError{
                .code = DatabaseErrorCode::QueryFailed,
                .message = QStringLiteral("Calendar projection requires a matching transaction"),
            };
        }

        auto& database = m_connection.database();
        std::optional<DatabaseError> failure;
        QSqlQuery removeEvent{database};
        removeEvent.prepare(QStringLiteral(
            "DELETE FROM calendar_events WHERE account_id=:account AND event_id=:event"));
        QSqlQuery resolveDestroyedInvitation{database};
        resolveDestroyedInvitation.prepare(
            QStringLiteral("UPDATE calendar_invitation_outbox SET status='resolved',resolved_at="
                           "COALESCE(resolved_at,CURRENT_TIMESTAMP) WHERE account_id=:account AND "
                           "event_id=:event AND status<>'resolved'"));
        QSqlQuery releaseDestroyedInvitation{database};
        releaseDestroyedInvitation.prepare(QStringLiteral(
            "DELETE FROM notification_dispatch_claims WHERE kind='invitation' AND claim_key IN "
            "(SELECT invitation_key FROM calendar_invitation_outbox WHERE account_id=:account "
            "AND event_id=:event)"));
        for (const auto& eventId : destroyedEventIds)
        {
            removeEvent.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(std::string{accountId}));
            removeEvent.bindValue(QStringLiteral(":event"), QString::fromStdString(eventId));
            if (!exec(removeEvent, failure, QStringLiteral("Project calendar event deletion")))
                return failure;
            resolveDestroyedInvitation.bindValue(QStringLiteral(":account"),
                                                 QString::fromStdString(std::string{accountId}));
            resolveDestroyedInvitation.bindValue(QStringLiteral(":event"),
                                                 QString::fromStdString(eventId));
            if (!exec(resolveDestroyedInvitation, failure,
                      QStringLiteral("Resolve destroyed calendar invitation")))
                return failure;
            releaseDestroyedInvitation.bindValue(QStringLiteral(":account"),
                                                 QString::fromStdString(std::string{accountId}));
            releaseDestroyedInvitation.bindValue(QStringLiteral(":event"),
                                                 QString::fromStdString(eventId));
            if (!exec(releaseDestroyedInvitation, failure,
                      QStringLiteral("Release destroyed invitation dispatch")))
                return failure;
        }

        QSqlQuery upsertEvent{database};
        upsertEvent.prepare(QStringLiteral(
            "INSERT INTO calendar_events (account_id,event_id,uid,title,description,location,"
            "document_json,state) VALUES (:account,:id,:uid,:title,:description,:location,"
            ":document,:state) ON CONFLICT(account_id,event_id) DO UPDATE SET uid=excluded.uid,"
            "title=excluded.title,description=excluded.description,location=excluded.location,"
            "document_json=excluded.document_json"));
        QSqlQuery clearMembership{database};
        clearMembership.prepare(QStringLiteral(
            "DELETE FROM calendar_event_calendars WHERE account_id=:account AND event_id=:event"));
        QSqlQuery addMembership{database};
        addMembership.prepare(QStringLiteral(
            "INSERT INTO calendar_event_calendars (account_id,event_id,calendar_id) VALUES "
            "(:account,:event,:calendar)"));
        QSqlQuery invitationOutbox{database};
        invitationOutbox.prepare(QStringLiteral(
            "SELECT recurrence_id,self_participant_id,invitation_key,source_notification_id FROM "
            "calendar_invitation_outbox WHERE account_id=:account AND event_id=:event"));
        QSqlQuery calendarRights{database};
        calendarRights.prepare(QStringLiteral("SELECT rights_json FROM calendars WHERE "
                                              "account_id=:account AND calendar_id=:calendar"));
        QSqlQuery restorePendingInvitation{database};
        restorePendingInvitation.prepare(QStringLiteral(
            "INSERT INTO calendar_pending_invitations(account_id,event_id,recurrence_id,"
            "self_participant_id,source_notification_id,display_recurrence_id,display_start) "
            "VALUES(:account,:event,:recurrence,:participant,:source,:display_recurrence,"
            ":display_start) ON CONFLICT(account_id,event_id,recurrence_id) DO UPDATE SET "
            "self_participant_id=excluded.self_participant_id,source_notification_id=COALESCE("
            "excluded.source_notification_id,calendar_pending_invitations.source_notification_id),"
            "display_recurrence_id=excluded.display_recurrence_id,display_start="
            "excluded.display_start,last_seen_at=CURRENT_TIMESTAMP"));
        QSqlQuery removePendingInvitation{database};
        removePendingInvitation.prepare(
            QStringLiteral("DELETE FROM calendar_pending_invitations WHERE account_id=:account AND "
                           "event_id=:event "
                           "AND recurrence_id=:recurrence"));
        QSqlQuery resolveInvitation{database};
        resolveInvitation.prepare(
            QStringLiteral("UPDATE calendar_invitation_outbox SET status='resolved',resolved_at="
                           "COALESCE(resolved_at,CURRENT_TIMESTAMP) WHERE invitation_key=:key AND "
                           "status<>'resolved'"));
        QSqlQuery releaseInvitation{database};
        releaseInvitation.prepare(QStringLiteral(
            "DELETE FROM notification_dispatch_claims WHERE kind='invitation' AND claim_key=:key"));
        for (const auto& event : events)
        {
            const auto document = api::serializeCalendarEventDocument(event);
            if (!document.has_value())
            {
                return DatabaseError{
                    .code = DatabaseErrorCode::QueryFailed,
                    .message = QStringLiteral("Serialize projected calendar event"),
                };
            }
            upsertEvent.bindValue(QStringLiteral(":account"),
                                  QString::fromStdString(std::string{accountId}));
            upsertEvent.bindValue(QStringLiteral(":id"), QString::fromStdString(event.id));
            upsertEvent.bindValue(QStringLiteral(":uid"), QString::fromStdString(event.uid));
            upsertEvent.bindValue(QStringLiteral(":title"), QString::fromStdString(event.title));
            upsertEvent.bindValue(QStringLiteral(":description"),
                                  optionalString(event.description));
            upsertEvent.bindValue(QStringLiteral(":location"), optionalString(event.location));
            upsertEvent.bindValue(QStringLiteral(":document"), QString::fromStdString(*document));
            upsertEvent.bindValue(QStringLiteral(":state"),
                                  QString::fromStdString(std::string{eventState}));
            if (!exec(upsertEvent, failure, QStringLiteral("Project calendar event")))
                return failure;
            clearMembership.bindValue(QStringLiteral(":account"),
                                      QString::fromStdString(std::string{accountId}));
            clearMembership.bindValue(QStringLiteral(":event"), QString::fromStdString(event.id));
            if (!exec(clearMembership, failure, QStringLiteral("Clear projected event calendars")))
                return failure;
            for (const auto& [calendarId, present] : event.calendarIds)
            {
                if (!present)
                    continue;
                addMembership.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(std::string{accountId}));
                addMembership.bindValue(QStringLiteral(":event"), QString::fromStdString(event.id));
                addMembership.bindValue(QStringLiteral(":calendar"),
                                        QString::fromStdString(calendarId));
                if (!exec(addMembership, failure, QStringLiteral("Add projected event calendar")))
                    return failure;
            }

            invitationOutbox.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(std::string{accountId}));
            invitationOutbox.bindValue(QStringLiteral(":event"), QString::fromStdString(event.id));
            if (!exec(invitationOutbox, failure, QStringLiteral("Read calendar invitation outbox")))
                return failure;
            struct InvitationOutboxRow
            {
                QString recurrenceText;
                std::string selfParticipantId;
                QString invitationKey;
                QVariant sourceNotification;
            };
            std::vector<InvitationOutboxRow> invitationRows;
            while (invitationOutbox.next())
            {
                invitationRows.push_back({
                    .recurrenceText = invitationOutbox.value(0).toString(),
                    .selfParticipantId = invitationOutbox.value(1).toString().toStdString(),
                    .invitationKey = invitationOutbox.value(2).toString(),
                    .sourceNotification = invitationOutbox.value(3),
                });
            }
            for (const auto& invitationRow : invitationRows)
            {
                const auto& recurrenceText = invitationRow.recurrenceText;
                const auto recurrenceId = recurrenceText.isEmpty()
                                              ? std::optional<calendar::LocalDateTime>{}
                                              : std::optional<calendar::LocalDateTime>{
                                                    {.value = recurrenceText.toStdString()}};
                const auto& selfParticipantId = invitationRow.selfParticipantId;
                const auto& invitationKey = invitationRow.invitationKey;
                const auto& sourceNotification = invitationRow.sourceNotification;

                std::optional<calendar::CalendarEvent> effectiveOccurrence;
                const calendar::CalendarEvent* effectiveEvent = &event;
                if (recurrenceId)
                {
                    effectiveOccurrence = calendar::effectiveOccurrenceEvent(event, *recurrenceId);
                    if (effectiveOccurrence)
                        effectiveEvent = &*effectiveOccurrence;
                    else
                        effectiveEvent = nullptr;
                }

                bool pendingInvitation =
                    effectiveEvent != nullptr && !effectiveEvent->isDraft && !effectiveEvent->isOrigin &&
                    effectiveEvent->status != std::optional<std::string>{"cancelled"};
                if (pendingInvitation)
                {
                    const auto participant = std::ranges::find(
                        effectiveEvent->attendees, selfParticipantId, &calendar::Attendee::id);
                    pendingInvitation = participant != effectiveEvent->attendees.end() &&
                                        !participant->isOwner &&
                                        participant->participationStatus == "needs-action";
                }

                bool hasCalendarMembership = false;
                if (pendingInvitation)
                {
                    for (const auto& [calendarId, present] : event.calendarIds)
                    {
                        if (!present)
                            continue;
                        hasCalendarMembership = true;
                        calendarRights.bindValue(QStringLiteral(":account"),
                                                 QString::fromStdString(std::string{accountId}));
                        calendarRights.bindValue(QStringLiteral(":calendar"),
                                                 QString::fromStdString(calendarId));
                        if (!exec(calendarRights, failure,
                                  QStringLiteral("Read invitation calendar rights")))
                            return failure;
                        if (!calendarRights.next() ||
                            (calendarRights.value(0).toUInt() & 32U) == 0U)
                        {
                            pendingInvitation = false;
                            break;
                        }
                    }
                    pendingInvitation = pendingInvitation && hasCalendarMembership;
                }

                if (pendingInvitation)
                {
                    restorePendingInvitation.bindValue(
                        QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
                    restorePendingInvitation.bindValue(QStringLiteral(":event"),
                                                       QString::fromStdString(event.id));
                    restorePendingInvitation.bindValue(QStringLiteral(":recurrence"),
                                                       recurrenceText);
                    restorePendingInvitation.bindValue(QStringLiteral(":participant"),
                                                       QString::fromStdString(selfParticipantId));
                    restorePendingInvitation.bindValue(QStringLiteral(":source"),
                                                       sourceNotification);
                    restorePendingInvitation.bindValue(QStringLiteral(":display_recurrence"),
                                                       recurrenceId ? QVariant{recurrenceText}
                                                                    : QVariant{});
                    restorePendingInvitation.bindValue(
                        QStringLiteral(":display_start"),
                        recurrenceId ? QVariant{QString::fromStdString(effectiveEvent->start.value)}
                                     : QVariant{});
                    if (!exec(restorePendingInvitation, failure,
                              QStringLiteral("Restore pending calendar invitation")))
                        return failure;
                    continue;
                }

                removePendingInvitation.bindValue(QStringLiteral(":account"),
                                                  QString::fromStdString(std::string{accountId}));
                removePendingInvitation.bindValue(QStringLiteral(":event"),
                                                  QString::fromStdString(event.id));
                removePendingInvitation.bindValue(QStringLiteral(":recurrence"), recurrenceText);
                if (!exec(removePendingInvitation, failure,
                          QStringLiteral("Project answered calendar invitation")))
                    return failure;
                resolveInvitation.bindValue(QStringLiteral(":key"), invitationKey);
                if (!exec(resolveInvitation, failure,
                          QStringLiteral("Resolve calendar invitation outbox")))
                    return failure;
                releaseInvitation.bindValue(QStringLiteral(":key"), invitationKey);
                if (!exec(releaseInvitation, failure,
                          QStringLiteral("Release calendar invitation dispatch")))
                    return failure;
            }
        }

        QSqlQuery removeOccurrences{database};
        removeOccurrences.prepare(QStringLiteral(
            "DELETE FROM calendar_occurrences WHERE account_id=:account AND event_id=:event"));
        QSqlQuery insertOccurrence{database};
        insertOccurrence.prepare(QStringLiteral(
            "INSERT INTO calendar_occurrences (account_id,occurrence_id,event_id,recurrence_id,"
            "start_utc,end_utc,local_start,local_end,is_all_day) VALUES "
            "(:account,:id,:event,NULL,:start_utc,:end_utc,:local_start,:local_end,:all_day)"));
        QSqlQuery addToWindows{database};
        addToWindows.prepare(QStringLiteral(
            "INSERT INTO calendar_window_occurrences (account_id,range_start,range_end,"
            "display_time_zone,occurrence_id) SELECT account_id,range_start,range_end,"
            "display_time_zone,:occurrence FROM calendar_query_windows WHERE account_id=:account "
            "AND range_start < :local_end AND range_end > :local_start"));
        for (const auto& occurrence : nonRecurringOccurrences)
        {
            removeOccurrences.bindValue(QStringLiteral(":account"),
                                        QString::fromStdString(std::string{accountId}));
            removeOccurrences.bindValue(QStringLiteral(":event"),
                                        QString::fromStdString(occurrence.eventId));
            if (!exec(removeOccurrences, failure,
                      QStringLiteral("Replace projected event occurrence")))
                return failure;
            insertOccurrence.bindValue(QStringLiteral(":account"),
                                       QString::fromStdString(std::string{accountId}));
            insertOccurrence.bindValue(QStringLiteral(":id"),
                                       QString::fromStdString(occurrence.id));
            insertOccurrence.bindValue(QStringLiteral(":event"),
                                       QString::fromStdString(occurrence.eventId));
            insertOccurrence.bindValue(
                QStringLiteral(":start_utc"),
                occurrence.utcStart ? QVariant{QString::fromStdString(occurrence.utcStart->value)}
                                    : QVariant{});
            insertOccurrence.bindValue(
                QStringLiteral(":end_utc"),
                occurrence.utcEnd ? QVariant{QString::fromStdString(occurrence.utcEnd->value)}
                                  : QVariant{});
            insertOccurrence.bindValue(QStringLiteral(":local_start"),
                                       QString::fromStdString(occurrence.localStart.value));
            insertOccurrence.bindValue(QStringLiteral(":local_end"),
                                       QString::fromStdString(occurrence.localEnd.value));
            insertOccurrence.bindValue(QStringLiteral(":all_day"), occurrence.allDay ? 1 : 0);
            if (!exec(insertOccurrence, failure,
                      QStringLiteral("Insert projected event occurrence")))
                return failure;
            addToWindows.bindValue(QStringLiteral(":occurrence"),
                                   QString::fromStdString(occurrence.id));
            addToWindows.bindValue(QStringLiteral(":account"),
                                   QString::fromStdString(std::string{accountId}));
            addToWindows.bindValue(QStringLiteral(":local_start"),
                                   QString::fromStdString(occurrence.localStart.value));
            addToWindows.bindValue(QStringLiteral(":local_end"),
                                   QString::fromStdString(occurrence.localEnd.value));
            if (!exec(addToWindows, failure, QStringLiteral("Add projected occurrence to windows")))
                return failure;
        }
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
        QSqlQuery touchWindow{m_connection.database()};
        touchWindow.prepare(QStringLiteral(
            "UPDATE calendar_query_windows SET "
            "updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE account_id=:account AND "
            "range_start=:start AND range_end=:end AND display_time_zone=:zone"));
        touchWindow.bindValue(QStringLiteral(":account"),
                              QString::fromStdString(std::string{accountId}));
        touchWindow.bindValue(QStringLiteral(":start"), QString::fromStdString(start.value));
        touchWindow.bindValue(QStringLiteral(":end"), QString::fromStdString(end.value));
        touchWindow.bindValue(QStringLiteral(":zone"),
                              QString::fromStdString(displayTimeZone.value));
        if (!touchWindow.exec())
            return queryError(QStringLiteral("Touch calendar window"), touchWindow);
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

    std::variant<CalendarWindow, DatabaseError> CalendarRepository::loadRangeSnapshot(
        const std::string_view accountId, const calendar::LocalDateTime& start,
        const calendar::LocalDateTime& end, const calendar::TimeZoneId& displayTimeZone) const
    {
        if (const auto error = m_connection.validate())
            return *error;

        std::string eventState;
        auto state = stateToken(accountId, "CalendarEvent");
        if (const auto* stateError = std::get_if<DatabaseError>(&state))
            return *stateError;
        if (const auto& value = std::get<std::optional<std::string>>(state); value.has_value())
            eventState = *value;

        CalendarWindow result{
            .accountId = std::string{accountId},
            .start = start,
            .end = end,
            .displayTimeZone = displayTimeZone,
            .queryState = {},
            .eventState = std::move(eventState),
            .events = {},
            .occurrences = {},
        };

        QSqlQuery query{m_connection.database()};
        query.prepare(QStringLiteral(
            "SELECT o.occurrence_id,o.event_id,o.recurrence_id,o.start_utc,o.end_utc,o.local_start,"
            "o.local_end,o.is_all_day,e.document_json FROM calendar_occurrences o JOIN "
            "calendar_events e ON e.account_id=o.account_id AND e.event_id=o.event_id WHERE "
            "o.account_id=:account AND o.local_start < :end AND o.local_end > :start ORDER BY "
            "o.local_start,o.local_end,o.occurrence_id"));
        query.bindValue(QStringLiteral(":account"), QString::fromStdString(std::string{accountId}));
        query.bindValue(QStringLiteral(":start"), QString::fromStdString(start.value));
        query.bindValue(QStringLiteral(":end"), QString::fromStdString(end.value));
        if (!query.exec())
            return queryError(QStringLiteral("Load calendar range snapshot"), query);

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
            if (!loadedEvents.insert(eventId).second)
                continue;
            const auto parsed =
                api::parseCalendarEventDocument(accountId, query.value(8).toString().toStdString());
            if (!parsed.ok() || !parsed.value.has_value())
                return DatabaseError{.code = DatabaseErrorCode::QueryFailed,
                                     .message = QStringLiteral("Parse cached calendar event")};
            result.events.push_back(*parsed.value);
        }
        return result;
    }
} // namespace javelin::jmap::cache
