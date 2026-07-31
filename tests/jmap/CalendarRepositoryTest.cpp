#include "jmap/cache/CalendarRepository.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/CalendarNotificationRepository.h"
#include "jmap/calendar/CalendarMutationJournal.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance())
                return;
            static int argc = 1;
            static char name[] = "calendar-repository-test";
            static char* argv[] = {name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    javelin::jmap::calendar::CalendarEvent event(std::string id, std::string start)
    {
        return {.accountId = "a1",
                .id = id,
                .baseEventId = std::nullopt,
                .recurrenceId = std::nullopt,
                .uid = "uid-" + id,
                .calendarIds = {{"work", true}},
                .title = "Event " + id,
                .description = std::nullopt,
                .location = std::nullopt,
                .start = {.value = std::move(start)},
                .duration = {.value = "PT1H"},
                .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"},
                .showWithoutTime = false,
                .isDraft = false,
                .isOrigin = true,
                .useDefaultAlerts = false,
                .alerts = {},
                .utcStart = std::nullopt,
                .utcEnd = std::nullopt,
                .recurrenceRule = std::nullopt,
                .recurrenceOverrides = {},
                .attendees = {}};
    }

    javelin::jmap::calendar::Occurrence occurrence(const std::string& id, const std::string& start)
    {
        return {.accountId = "a1",
                .id = id,
                .eventId = id,
                .recurrenceId = std::nullopt,
                .localStart = {.value = start},
                .localEnd = {.value = start},
                .utcStart = std::nullopt,
                .utcEnd = std::nullopt,
                .allDay = false};
    }
} // namespace

TEST_CASE("calendar create and delete projections render effective state",
          "[jmap][calendar][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-object-projections"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery account{connection.database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts (account_id,email_address,session_url,is_primary) VALUES "
        "('a1','alice@example.test','https://example.test/jmap',1)")));
    javelin::jmap::cache::CalendarRepository repository{connection};
    const javelin::jmap::calendar::Calendar work{
        .accountId = "a1",
        .id = "work",
        .name = "Work",
        .description = std::nullopt,
        .color = "#2457a6",
        .sortOrder = 0,
        .isSubscribed = true,
        .isVisible = true,
        .isDefault = true,
        .timeZone = std::nullopt,
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {.mayReadFreeBusy = true,
                     .mayReadItems = true,
                     .mayWriteAll = true,
                     .mayWriteOwn = true,
                     .mayUpdatePrivate = true,
                     .mayRSVP = true,
                     .mayShare = false,
                     .mayDelete = true},
    };
    REQUIRE_FALSE(repository.replaceCalendars("a1", "c1", {work}).has_value());

    auto deleteResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Project Calendar deletion"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(deleteResult));
    auto deletion = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(deleteResult));
    REQUIRE_FALSE(
        repository.projectCalendarDeletion(deletion, "a1", "work", "delete-1").has_value());
    REQUIRE_FALSE(deletion.commit().has_value());
    auto listed = repository.listCalendars("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::Calendar>>(listed));
    CHECK(std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed).empty());

    auto rejectResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Reject Calendar deletion"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(rejectResult));
    auto rejection = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(rejectResult));
    REQUIRE_FALSE(repository.clearCalendarDeletion(rejection, "delete-1").has_value());
    REQUIRE_FALSE(rejection.commit().has_value());
    listed = repository.listCalendars("a1");
    REQUIRE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed).size() == 1);

    auto pending = work;
    pending.id = "pending-create";
    pending.name = "Projects";
    pending.isDefault = false;
    auto createResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Project Calendar creation"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(createResult));
    auto creation = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(createResult));
    REQUIRE_FALSE(repository.projectCalendarCreation(creation, "a1", "c1", pending).has_value());
    REQUIRE_FALSE(creation.commit().has_value());
    listed = repository.listCalendars("a1");
    REQUIRE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed).size() == 2);

    auto acceptResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Accept Calendar creation"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(acceptResult));
    auto acceptance = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(acceptResult));
    REQUIRE_FALSE(
        repository
            .acceptProjectedCalendar(acceptance, "a1", "pending-create", "projects", "c2", false)
            .has_value());
    REQUIRE_FALSE(acceptance.commit().has_value());
    listed = repository.listCalendars("a1");
    const auto& accepted = std::get<std::vector<javelin::jmap::calendar::Calendar>>(listed);
    CHECK(std::ranges::find(accepted, "projects", &javelin::jmap::calendar::Calendar::id) !=
          accepted.end());
    CHECK(std::ranges::find(accepted, "pending-create", &javelin::jmap::calendar::Calendar::id) ==
          accepted.end());
}

TEST_CASE("calendar windows retain occurrences referenced by overlapping windows",
          "[jmap][calendar][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-window-overlap"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery account{connection.database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts (account_id,email_address,session_url,is_primary) VALUES "
        "('a1','alice@example.test','https://example.test/jmap',1)")));

    javelin::jmap::cache::CalendarRepository repository{connection};
    javelin::jmap::calendar::Calendar calendar{
        .accountId = "a1",
        .id = "work",
        .name = "Work",
        .description = std::nullopt,
        .color = "#2457a6",
        .sortOrder = 0,
        .isSubscribed = true,
        .isVisible = true,
        .isDefault = true,
        .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"},
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {.mayReadFreeBusy = true,
                     .mayReadItems = true,
                     .mayWriteAll = true,
                     .mayWriteOwn = true,
                     .mayUpdatePrivate = true,
                     .mayRSVP = true,
                     .mayShare = false,
                     .mayDelete = false}};
    REQUIRE_FALSE(repository.replaceCalendars("a1", "c1", {calendar}).has_value());
    REQUIRE_FALSE(repository.setCalendarVisible("a1", "work", false).has_value());
    auto hiddenCalendars = repository.listCalendars("a1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars));
    REQUIRE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars).size() == 1);
    CHECK_FALSE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars)
                    .front()
                    .isVisible);
    auto serverChangedCalendar = calendar;
    serverChangedCalendar.isDefault = false;
    REQUIRE_FALSE(
        repository.replaceCalendars("a1", "c1-refreshed", {serverChangedCalendar}).has_value());
    hiddenCalendars = repository.listCalendars("a1");
    CHECK_FALSE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars)
                    .front()
                    .isVisible);
    CHECK_FALSE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars)
                    .front()
                    .isDefault);
    auto defaultTransactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Test server calendar default"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(
        defaultTransactionResult));
    auto defaultTransaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(defaultTransactionResult));
    REQUIRE_FALSE(
        repository.applyCalendarDefaults(defaultTransaction, "a1", "c1-default", {{"work", true}})
            .has_value());
    REQUIRE_FALSE(defaultTransaction.commit().has_value());
    hiddenCalendars = repository.listCalendars("a1");
    CHECK(std::get<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars)
              .front()
              .isDefault);

    const javelin::jmap::calendar::TimeZoneId zone{.value = "Pacific/Auckland"};
    const javelin::jmap::cache::CalendarWindow first{
        .accountId = "a1",
        .start = {.value = "2026-03-01T00:00:00"},
        .end = {.value = "2026-04-12T00:00:00"},
        .displayTimeZone = zone,
        .queryState = "q1",
        .eventState = "e1-state",
        .events = {event("e1", "2026-03-04T09:00:00"), event("e2", "2026-04-02T09:00:00")},
        .occurrences = {occurrence("e1", "2026-03-04T09:00:00"),
                        occurrence("e2", "2026-04-02T09:00:00")}};
    REQUIRE_FALSE(repository.reconcileWindow(first).has_value());
    const javelin::jmap::cache::CalendarWindow second{
        .accountId = "a1",
        .start = {.value = "2026-03-29T00:00:00"},
        .end = {.value = "2026-05-10T00:00:00"},
        .displayTimeZone = zone,
        .queryState = "q2",
        .eventState = "e2-state",
        .events = {event("e2", "2026-04-02T09:00:00")},
        .occurrences = {occurrence("e2", "2026-04-02T09:00:00")}};
    REQUIRE_FALSE(repository.reconcileWindow(second).has_value());

    const auto foundEvent = repository.findEvent("a1", "e1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(foundEvent));
    REQUIRE(
        std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(foundEvent).has_value());
    CHECK(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(foundEvent)->title ==
          "Event e1");

    auto projectedEvent = event("e1", "2026-03-04T10:00:00");
    projectedEvent.title = "Projected event";
    auto projectedOccurrence = occurrence("e1", "2026-03-04T10:00:00");
    projectedOccurrence.localEnd = {.value = "2026-03-04T11:00:00"};
    auto projectResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Project calendar event"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(projectResult));
    auto projection = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(projectResult));
    REQUIRE_FALSE(repository
                      .projectEvents(projection, "a1", "e1-state", {projectedEvent},
                                     {projectedOccurrence}, {})
                      .has_value());
    REQUIRE_FALSE(projection.commit().has_value());
    const auto projectedWindow =
        repository.loadWindow("a1", first.start, first.end, first.displayTimeZone);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(
        projectedWindow));
    const auto& projectedValue =
        std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(projectedWindow);
    REQUIRE(projectedValue.has_value());
    REQUIRE(projectedValue->events.size() == 2);
    const auto projectedItem = std::ranges::find(projectedValue->events, "e1",
                                                 &javelin::jmap::calendar::CalendarEvent::id);
    REQUIRE(projectedItem != projectedValue->events.end());
    CHECK(projectedItem->title == "Projected event");
    const auto projectedInstance = std::ranges::find(projectedValue->occurrences, "e1",
                                                     &javelin::jmap::calendar::Occurrence::id);
    REQUIRE(projectedInstance != projectedValue->occurrences.end());
    CHECK(projectedInstance->localStart.value == "2026-03-04T10:00:00");

    auto journalEvent = projectedEvent;
    journalEvent.title = "Journal event";
    const auto baseDocument = javelin::jmap::api::serializeCalendarEventDocument(projectedEvent);
    const auto projectedDocument = javelin::jmap::api::serializeCalendarEventDocument(journalEvent);
    REQUIRE(baseDocument.has_value());
    REQUIRE(projectedDocument.has_value());
    const javelin::jmap::calendar::CalendarMutationRecord mutation{
        .mutationId = "calendar-update-1",
        .operationGroupId = std::nullopt,
        .accountId = "a1",
        .objectId = "e1",
        .creationId = std::nullopt,
        .kind = javelin::jmap::calendar::CalendarMutationKind::Update,
        .status = javelin::jmap::sync::MutationStatus::Pending,
        .requestedDocument = *projectedDocument,
        .baseDocument = *baseDocument,
        .projectedDocument = *projectedDocument,
        .baseState = "e1-state",
        .acceptedState = std::nullopt,
        .errorJson = std::nullopt,
    };
    javelin::jmap::calendar::CalendarMutationJournal journal{connection, repository};
    REQUIRE_FALSE(journal.queue({mutation}, "e1-state", {journalEvent}, {projectedOccurrence}, {})
                      .has_value());
    const auto journalRecords = journal.listForEvent("a1", "e1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(
        journalRecords));
    REQUIRE(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(journalRecords)
                .size() == 1);
    CHECK(std::get<std::vector<javelin::jmap::calendar::CalendarMutationRecord>>(journalRecords)
              .front()
              .status == javelin::jmap::sync::MutationStatus::Pending);
    REQUIRE_FALSE(
        journal.transition({mutation}, javelin::jmap::sync::MutationStatus::InFlight).has_value());
    const auto journalProjected = repository.findEvent("a1", "e1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(
        journalProjected));
    REQUIRE(std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(journalProjected)
                .has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(journalProjected)->title ==
        "Journal event");

    auto rollbackResult = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Rollback calendar projection"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(rollbackResult));
    auto rollback = std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(rollbackResult));
    const std::array destroyed{std::string{"e2"}};
    REQUIRE_FALSE(
        repository.projectEvents(rollback, "a1", "e1-state", {}, {}, destroyed).has_value());
    rollback.rollback();
    const auto retainedEvent = repository.findEvent("a1", "e2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::calendar::CalendarEvent>>(
        retainedEvent));
    CHECK(
        std::get<std::optional<javelin::jmap::calendar::CalendarEvent>>(retainedEvent).has_value());

    auto refreshedFirst = first;
    refreshedFirst.queryState = "q3";
    refreshedFirst.events.clear();
    refreshedFirst.occurrences.clear();
    REQUIRE_FALSE(repository.reconcileWindow(refreshedFirst).has_value());

    const auto loadedSecond =
        repository.loadWindow("a1", second.start, second.end, second.displayTimeZone);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(loadedSecond));
    const auto& value = std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(loadedSecond);
    REQUIRE(value.has_value());
    REQUIRE(value->occurrences.size() == 1);
    CHECK(value->occurrences.front().id == "e2");
    CHECK(value->eventState == "e1-state");

    const auto calendarState = repository.stateToken("a1", "Calendar");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(calendarState));
    CHECK(std::get<std::optional<std::string>>(calendarState) ==
          std::optional<std::string>{"c1-default"});
    const auto eventState = repository.stateToken("a1", "CalendarEvent");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(eventState));
    CHECK(std::get<std::optional<std::string>>(eventState) ==
          std::optional<std::string>{"e1-state"});

    QSqlQuery count{connection.database()};
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_occurrences")));
    REQUIRE(count.next());
    CHECK(count.value(0).toInt() == 1);

    auto changedEvent = event("e2", "2026-04-02T10:00:00");
    changedEvent.title = "Changed event";
    const auto changedOccurrence = occurrence("e2", "2026-04-02T10:00:00");
    REQUIRE_FALSE(
        repository
            .applyEventDelta("a1", "c2", "e3-state", zone, {changedEvent}, {changedOccurrence}, {})
            .has_value());
    const auto updatedFirst =
        repository.loadWindow("a1", first.start, first.end, first.displayTimeZone);
    const auto updatedSecond =
        repository.loadWindow("a1", second.start, second.end, second.displayTimeZone);
    REQUIRE(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(updatedFirst)
                ->events.size() == 1);
    CHECK(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(updatedFirst)
              ->events.front()
              .title == "Changed event");
    REQUIRE(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(updatedSecond)
                ->events.size() == 1);
    CHECK(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(updatedSecond)
              ->events.front()
              .title == "Changed event");

    REQUIRE_FALSE(
        repository.applyEventDelta("a1", "c2", "e4-state", zone, {}, {}, {"e2"}).has_value());
    const auto destroyedFirst =
        repository.loadWindow("a1", first.start, first.end, first.displayTimeZone);
    const auto destroyedSecond =
        repository.loadWindow("a1", second.start, second.end, second.displayTimeZone);
    CHECK(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(destroyedFirst)
              ->events.empty());
    CHECK(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(destroyedSecond)
              ->events.empty());

    REQUIRE(count.exec(
        QStringLiteral("UPDATE calendar_query_windows SET updated_at=CASE range_start WHEN "
                       "'2026-03-01T00:00:00' THEN '2000-01-01T00:00:00.000Z' ELSE "
                       "'2001-01-01T00:00:00.000Z' END")));
    const auto touchedFirst =
        repository.loadWindow("a1", first.start, first.end, first.displayTimeZone);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(touchedFirst));
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(touchedFirst).has_value());
    for (auto month = 1; month <= 11; ++month)
    {
        const auto start =
            QStringLiteral("2030-%1-01T00:00:00").arg(month, 2, 10, QLatin1Char('0'));
        const auto end = QStringLiteral("2030-%1-28T00:00:00").arg(month, 2, 10, QLatin1Char('0'));
        const javelin::jmap::cache::CalendarWindow window{.accountId = "a1",
                                                          .start = {.value = start.toStdString()},
                                                          .end = {.value = end.toStdString()},
                                                          .displayTimeZone = zone,
                                                          .queryState =
                                                              "bounded-" + std::to_string(month),
                                                          .eventState = "e4-state",
                                                          .events = {},
                                                          .occurrences = {}};
        REQUIRE_FALSE(repository.reconcileWindow(window).has_value());
    }

    const auto retainedFirst =
        repository.loadWindow("a1", first.start, first.end, first.displayTimeZone);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(retainedFirst));
    CHECK(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(retainedFirst).has_value());
    const auto evictedSecond =
        repository.loadWindow("a1", second.start, second.end, second.displayTimeZone);
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(evictedSecond));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(evictedSecond).has_value());
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_query_windows")));
    REQUIRE(count.next());
    CHECK(count.value(0).toInt() == 12);
}

TEST_CASE("calendar reminders are claimed once and can be snoozed or dismissed",
          "[jmap][calendar][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-state"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    REQUIRE(QSqlQuery{connection.database()}.exec(QStringLiteral(
        "INSERT INTO accounts (account_id,email_address,session_url,is_primary) VALUES "
        "('a1','alice@example.test','https://example.test/jmap',1)")));

    javelin::jmap::cache::CalendarRepository calendars{connection};
    javelin::jmap::calendar::Calendar calendar{
        .accountId = "a1",
        .id = "work",
        .name = "Work",
        .description = std::nullopt,
        .color = std::nullopt,
        .sortOrder = 0,
        .isSubscribed = true,
        .isVisible = true,
        .isDefault = true,
        .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"},
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {.mayReadFreeBusy = true, .mayReadItems = true}};
    calendar.defaultAlertsWithTime.emplace(
        "default-alert", javelin::jmap::calendar::Alert{
                             .id = "default-alert",
                             .action = "display",
                             .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                             .relativeTo = "start",
                             .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                             .when = std::nullopt,
                             .acknowledged = std::nullopt});
    REQUIRE_FALSE(calendars.replaceCalendars("a1", "c1", {calendar}).has_value());

    auto remindedEvent = event("event-1", "2026-07-18T10:00:00");
    remindedEvent.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"};
    remindedEvent.utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T10:00:00Z"};
    remindedEvent.utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T11:00:00Z"};
    remindedEvent.alerts.emplace(
        "alert-1", javelin::jmap::calendar::Alert{
                       .id = "alert-1",
                       .action = "display",
                       .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                       .relativeTo = "start",
                       .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                       .when = std::nullopt,
                       .acknowledged = std::nullopt});
    auto remindedOccurrence = occurrence("event-1", "2026-07-18T10:00:00");
    remindedOccurrence.localEnd = {.value = "2026-07-18T11:00:00"};
    remindedOccurrence.utcStart =
        javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T10:00:00Z"};
    remindedOccurrence.utcEnd =
        javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T11:00:00Z"};
    REQUIRE_FALSE(calendars
                      .reconcileWindow({.accountId = "a1",
                                        .start = {.value = "2026-07-01T00:00:00"},
                                        .end = {.value = "2026-08-01T00:00:00"},
                                        .displayTimeZone = {.value = "Etc/UTC"},
                                        .queryState = "q1",
                                        .eventState = "e1",
                                        .events = {remindedEvent},
                                        .occurrences = {remindedOccurrence}})
                      .has_value());

    javelin::jmap::cache::CalendarNotificationRepository notifications{connection};
    const QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-07-18T10:05:00Z"), Qt::ISODate);
    auto first = notifications.claimDue(now);
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
            first));
    auto candidates =
        std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(first);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().title == "Event event-1");
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
              notifications.claimDue(now))
              .empty());
    REQUIRE_FALSE(notifications.markDelivered(candidates.front().key, now).has_value());

    REQUIRE_FALSE(notifications.snooze(candidates.front().key, now.addSecs(300)).has_value());
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
              notifications.claimDue(now.addSecs(299)))
              .empty());
    auto snoozed = notifications.claimDue(now.addSecs(300));
    REQUIRE(std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(snoozed)
                .size() == 1);
    REQUIRE_FALSE(
        notifications.markDelivered(candidates.front().key, now.addSecs(300)).has_value());
    REQUIRE_FALSE(notifications.dismiss(candidates.front().key).has_value());
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
              notifications.claimDue(now.addSecs(600)))
              .empty());

    remindedEvent.alerts.at("alert-1").offset = javelin::jmap::calendar::Duration{.value = "-PT5M"};
    REQUIRE_FALSE(calendars
                      .reconcileWindow({.accountId = "a1",
                                        .start = {.value = "2026-07-01T00:00:00"},
                                        .end = {.value = "2026-08-01T00:00:00"},
                                        .displayTimeZone = {.value = "Etc/UTC"},
                                        .queryState = "q2",
                                        .eventState = "e2",
                                        .events = {remindedEvent},
                                        .occurrences = {remindedOccurrence}})
                      .has_value());
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
              notifications.claimDue(now.addSecs(600)))
              .size() == 1);

    remindedEvent.useDefaultAlerts = true;
    remindedEvent.alerts.clear();
    REQUIRE_FALSE(calendars
                      .reconcileWindow({.accountId = "a1",
                                        .start = {.value = "2026-07-01T00:00:00"},
                                        .end = {.value = "2026-08-01T00:00:00"},
                                        .displayTimeZone = {.value = "Etc/UTC"},
                                        .queryState = "q3",
                                        .eventState = "e3",
                                        .events = {remindedEvent},
                                        .occurrences = {remindedOccurrence}})
                      .has_value());
    const auto defaultReminder = notifications.claimDue(now.addSecs(600));
    REQUIRE(
        std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(defaultReminder)
            .size() == 1);
    CHECK(
        std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(defaultReminder)
            .front()
            .alertId == "default-alert");
}

TEST_CASE("calendar reminders use their actual trigger rather than occurrence start bounds",
          "[jmap][calendar][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-trigger-time"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    REQUIRE(QSqlQuery{connection.database()}.exec(QStringLiteral(
        "INSERT INTO accounts (account_id,email_address,session_url,is_primary) VALUES "
        "('a1','alice@example.test','https://example.test/jmap',1)")));

    javelin::jmap::cache::CalendarRepository calendars{connection};
    const javelin::jmap::calendar::Calendar calendar{
        .accountId = "a1",
        .id = "work",
        .name = "Work",
        .description = std::nullopt,
        .color = std::nullopt,
        .sortOrder = 0,
        .isSubscribed = true,
        .isVisible = true,
        .isDefault = true,
        .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"},
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {.mayReadFreeBusy = true, .mayReadItems = true}};
    REQUIRE_FALSE(calendars.replaceCalendars("a1", "c1", {calendar}).has_value());

    auto longEvent = event("long-event", "2026-07-14T10:00:00");
    longEvent.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"};
    longEvent.duration = {.value = "P4D"};
    longEvent.alerts.emplace("before-end",
                             javelin::jmap::calendar::Alert{
                                 .id = "before-end",
                                 .action = "display",
                                 .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                                 .relativeTo = "end",
                                 .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                                 .when = std::nullopt,
                                 .acknowledged = std::nullopt});
    auto longOccurrence = occurrence("long-event", "2026-07-14T10:00:00");
    longOccurrence.localEnd = {.value = "2026-07-18T10:00:00"};
    longOccurrence.utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-07-14T10:00:00Z"};
    longOccurrence.utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T10:00:00Z"};

    auto absoluteEvent = event("future-event", "2028-07-18T10:00:00");
    absoluteEvent.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"};
    absoluteEvent.useDefaultAlerts = true;
    absoluteEvent.alerts.emplace(
        "absolute",
        javelin::jmap::calendar::Alert{
            .id = "absolute",
            .action = "display",
            .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Absolute,
            .relativeTo = "start",
            .offset = std::nullopt,
            .when = javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T09:50:00Z"},
            .acknowledged = std::nullopt});
    auto firstFutureOccurrence = occurrence("future-event", "2028-07-18T10:00:00");
    firstFutureOccurrence.id = "future-event:first";
    firstFutureOccurrence.utcStart =
        javelin::jmap::calendar::UtcInstant{.value = "2028-07-18T10:00:00Z"};
    firstFutureOccurrence.utcEnd =
        javelin::jmap::calendar::UtcInstant{.value = "2028-07-18T11:00:00Z"};
    auto secondFutureOccurrence = firstFutureOccurrence;
    secondFutureOccurrence.id = "future-event:second";
    secondFutureOccurrence.localStart = {.value = "2028-07-19T10:00:00"};
    secondFutureOccurrence.localEnd = {.value = "2028-07-19T11:00:00"};
    secondFutureOccurrence.utcStart =
        javelin::jmap::calendar::UtcInstant{.value = "2028-07-19T10:00:00Z"};
    secondFutureOccurrence.utcEnd =
        javelin::jmap::calendar::UtcInstant{.value = "2028-07-19T11:00:00Z"};

    REQUIRE_FALSE(calendars
                      .reconcileWindow({.accountId = "a1",
                                        .start = {.value = "2026-01-01T00:00:00"},
                                        .end = {.value = "2029-01-01T00:00:00"},
                                        .displayTimeZone = {.value = "Etc/UTC"},
                                        .queryState = "q1",
                                        .eventState = "e1",
                                        .events = {longEvent, absoluteEvent},
                                        .occurrences = {longOccurrence, firstFutureOccurrence,
                                                        secondFutureOccurrence}})
                      .has_value());

    javelin::jmap::cache::CalendarNotificationRepository notifications{connection};
    const auto trigger = QDateTime::fromString(QStringLiteral("2026-07-18T09:50:00Z"), Qt::ISODate);
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
              notifications.claimDue(trigger.addSecs(-1)))
              .empty());
    const auto due = notifications.claimDue(trigger);
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
            due));
    const auto& candidates =
        std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(due);
    REQUIRE(candidates.size() == 2);
    CHECK(std::ranges::count(candidates, std::string{"before-end"},
                             &javelin::jmap::cache::CalendarNotificationCandidate::alertId) == 1);
    CHECK(std::ranges::count(candidates, std::string{"absolute"},
                             &javelin::jmap::cache::CalendarNotificationCandidate::alertId) == 1);
}
