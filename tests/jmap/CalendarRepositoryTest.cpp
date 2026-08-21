#include "jmap/cache/CalendarRepository.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/CalendarInvitationRepository.h"
#include "jmap/cache/CalendarNotificationRepository.h"
#include "jmap/calendar/CalendarCacheReader.h"
#include "jmap/calendar/CalendarEventEditing.h"
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

TEST_CASE("calendar range snapshots do not depend on query-window membership",
          "[jmap][calendar][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-range-snapshot"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    REQUIRE(QSqlQuery{connection.database()}.exec(QStringLiteral(
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
    REQUIRE_FALSE(repository.replaceCalendars("a1", "calendar-1", {work}).has_value());
    const javelin::jmap::calendar::TimeZoneId zone{.value = "Pacific/Auckland"};
    const javelin::jmap::cache::CalendarWindow window{
        .accountId = "a1",
        .start = {.value = "2026-08-01T00:00:00"},
        .end = {.value = "2026-09-01T00:00:00"},
        .displayTimeZone = zone,
        .queryState = "query-1",
        .eventState = "event-1",
        .events = {event("e1", "2026-08-15T09:00:00"), event("e2", "2026-08-15T11:00:00")},
        .occurrences = {occurrence("e1", "2026-08-15T09:00:00"),
                        occurrence("e2", "2026-08-15T11:00:00")},
    };
    REQUIRE_FALSE(repository.reconcileWindow(window).has_value());

    QSqlQuery clearMembership{connection.database()};
    REQUIRE(clearMembership.exec(QStringLiteral("DELETE FROM calendar_window_occurrences")));

    const auto exact = repository.loadWindow("a1", window.start, window.end, zone);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::CalendarWindow>>(exact));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(exact).has_value());
    CHECK(
        std::get<std::optional<javelin::jmap::cache::CalendarWindow>>(exact)->occurrences.empty());

    const auto snapshot = repository.loadRangeSnapshot("a1", {.value = "2026-08-15T00:00:00"},
                                                       {.value = "2026-08-16T00:00:00"}, zone);
    REQUIRE(std::holds_alternative<javelin::jmap::cache::CalendarWindow>(snapshot));
    const auto& range = std::get<javelin::jmap::cache::CalendarWindow>(snapshot);
    REQUIRE(range.events.size() == 2);
    REQUIRE(range.occurrences.size() == 2);
    CHECK(range.events[0].id == "e1");
    CHECK(range.events[1].id == "e2");
    CHECK(range.occurrences[0].eventId == "e1");
    CHECK(range.occurrences[1].eventId == "e2");
    CHECK(range.eventState == "event-1");
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
    absoluteEvent.useDefaultAlerts = false;
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

    auto defaultlessEvent = event("defaultless-event", "2026-07-18T10:00:00");
    defaultlessEvent.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"};
    defaultlessEvent.useDefaultAlerts = true;
    defaultlessEvent.alerts.emplace(
        "ignored-event-alert", javelin::jmap::calendar::Alert{
                                   .id = "ignored-event-alert",
                                   .action = "display",
                                   .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
                                   .relativeTo = "start",
                                   .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
                                   .when = std::nullopt,
                                   .acknowledged = std::nullopt});
    auto defaultlessOccurrence = occurrence("defaultless-event", "2026-07-18T10:00:00");
    defaultlessOccurrence.utcStart =
        javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T10:00:00Z"};
    defaultlessOccurrence.utcEnd =
        javelin::jmap::calendar::UtcInstant{.value = "2026-07-18T11:00:00Z"};

    REQUIRE_FALSE(
        calendars
            .reconcileWindow({.accountId = "a1",
                              .start = {.value = "2026-01-01T00:00:00"},
                              .end = {.value = "2029-01-01T00:00:00"},
                              .displayTimeZone = {.value = "Etc/UTC"},
                              .queryState = "q1",
                              .eventState = "e1",
                              .events = {longEvent, absoluteEvent, defaultlessEvent},
                              .occurrences = {longOccurrence, firstFutureOccurrence,
                                              secondFutureOccurrence, defaultlessOccurrence}})
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
    CHECK(std::ranges::count(candidates, std::string{"ignored-event-alert"},
                             &javelin::jmap::cache::CalendarNotificationCandidate::alertId) == 0);
}

TEST_CASE("calendar invitations reconcile atomically and rejected RSVP does not alert twice",
          "[jmap][calendar][invitation][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-invitation-reconciliation"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery account{connection.database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts (account_id,email_address,session_url,is_primary) VALUES "
        "('a1','alice@example.test','https://example.test/jmap',1)")));

    javelin::jmap::cache::CalendarRepository calendars{connection};
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
        .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"},
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {.mayReadFreeBusy = true,
                     .mayReadItems = true,
                     .mayWriteAll = false,
                     .mayWriteOwn = false,
                     .mayUpdatePrivate = false,
                     .mayRSVP = true,
                     .mayShare = false,
                     .mayDelete = false},
    };
    auto noRsvp = work;
    noRsvp.id = "reference";
    noRsvp.name = "Reference";
    noRsvp.isDefault = false;
    noRsvp.myRights.mayRSVP = false;
    REQUIRE_FALSE(calendars.replaceCalendars("a1", "c1", {work, noRsvp}).has_value());

    auto invitation = event("invite-1", "2000-08-20T10:00:00");
    invitation.calendarIds = {{"work", true}, {"reference", true}};
    invitation.isOrigin = false;
    invitation.recurrenceRule = javelin::jmap::calendar::RecurrenceRule{};
    invitation.recurrenceRule->frequency = javelin::jmap::calendar::RecurrenceFrequency::Weekly;
    invitation.attendees = {{.id = "organizer",
                             .name = "Organizer",
                             .email = "organizer@example.test",
                             .calendarAddress = "mailto:organizer@example.test",
                             .participationStatus = "accepted",
                             .isOwner = true,
                             .isAttendee = true,
                             .roles = {{"owner", true}, {"attendee", true}},
                             .expectReply = false,
                             .scheduleSequence = 0,
                             .scheduleUpdated = std::nullopt},
                            {.id = "self",
                             .name = "Alice",
                             .email = "alice@example.test",
                             .calendarAddress = "mailto:alice@example.test",
                             .participationStatus = "needs-action",
                             .isOwner = false,
                             .isAttendee = true,
                             .roles = {{"attendee", true}},
                             .expectReply = false,
                             .scheduleSequence = 0,
                             .scheduleUpdated = std::nullopt}};
    invitation.organizerCalendarAddress = "mailto:organizer@example.test";
    const javelin::jmap::calendar::Occurrence futureOccurrence{
        .accountId = "a1",
        .id = "invite-1:future",
        .eventId = "invite-1",
        .recurrenceId = javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
        .localStart = {.value = "2099-08-20T10:00:00"},
        .localEnd = {.value = "2099-08-20T11:00:00"},
        .utcStart = javelin::jmap::calendar::UtcInstant{.value = "2099-08-19T22:00:00Z"},
        .utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2099-08-19T23:00:00Z"},
        .allDay = false,
    };
    REQUIRE_FALSE(calendars
                      .reconcileWindow({.accountId = "a1",
                                        .start = {.value = "2099-08-01T00:00:00"},
                                        .end = {.value = "2099-09-01T00:00:00"},
                                        .displayTimeZone = {.value = "Pacific/Auckland"},
                                        .queryState = "q1",
                                        .eventState = "e0",
                                        .events = {invitation},
                                        .occurrences = {futureOccurrence}})
                      .has_value());

    const javelin::jmap::calendar::CalendarEventNotification notification{
        .accountId = "a1",
        .id = "notification-1",
        .created = {.value = "2026-08-14T01:02:03Z"},
        .changedBy = {.name = "Organizer",
                      .email = "organizer@example.test",
                      .principalId = std::nullopt,
                      .calendarAddress = "mailto:organizer@example.test"},
        .comment = std::nullopt,
        .type = javelin::jmap::calendar::CalendarEventNotificationType::Created,
        .calendarEventId = "invite-1",
        .isDraft = false,
        .event = invitation,
        .eventPatchJson = std::nullopt,
    };

    javelin::jmap::cache::CalendarInvitationRepository invitations{connection};
    REQUIRE_FALSE(
        invitations
            .replaceParticipantIdentities("a1", "p1",
                                          {{.id = "primary",
                                            .name = "Alice",
                                            .calendarAddress = "mailto:alice@example.test",
                                            .isDefault = true},
                                           {.id = "alias",
                                            .name = "Alice Alias",
                                            .calendarAddress = "mailto:alias@example.test",
                                            .isDefault = false}})
            .has_value());
    javelin::jmap::calendar::CalendarCacheReader cacheReader{connection};
    const auto identityRead = cacheReader.participantIdentities("a1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::calendar::ParticipantIdentity>>(
        identityRead));
    const auto& identities =
        std::get<std::vector<javelin::jmap::calendar::ParticipantIdentity>>(identityRead);
    REQUIRE(identities.size() == 2);
    CHECK(identities.front().id == "primary");
    CHECK(identities.back().calendarAddress == "mailto:alias@example.test");
    const auto participantIdentityState = calendars.stateToken("a1", "ParticipantIdentity");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(participantIdentityState));
    REQUIRE(std::get<std::optional<std::string>>(participantIdentityState).has_value());
    CHECK(*std::get<std::optional<std::string>>(participantIdentityState) == "p1");

    REQUIRE_FALSE(invitations
                      .reconcile({.accountId = "a1",
                                  .notificationState = "n1",
                                  .eventState = "e1",
                                  .replaceNotifications = true,
                                  .notifications = {notification},
                                  .deletedNotificationIds = {},
                                  .events = {invitation},
                                  .nonRecurringOccurrences = {},
                                  .destroyedEventIds = {},
                                  .consideredEventIds = {"invite-1"},
                                  .pendingInvitations = {{.eventId = "invite-1",
                                                          .selfParticipantId = "self",
                                                          .sourceNotificationId = "notification-1",
                                                          .enqueueDesktopNotification = true}}})
                      .has_value());

    auto state = calendars.stateToken("a1", "CalendarEventNotification");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(state));
    CHECK(std::get<std::optional<std::string>>(state) == std::optional<std::string>{"n1"});
    QSqlQuery projected{connection.database()};
    REQUIRE(projected.exec(QStringLiteral(
        "SELECT p.self_participant_id,o.status FROM calendar_pending_invitations p JOIN "
        "calendar_invitation_outbox o ON o.account_id=p.account_id AND o.event_id=p.event_id "
        "WHERE p.account_id='a1' AND p.event_id='invite-1'")));
    REQUIRE(projected.next());
    CHECK(projected.value(0).toString() == QStringLiteral("self"));
    CHECK(projected.value(1).toString() == QStringLiteral("pending"));

    const auto firstClaim = invitations.claimPendingDispatches();
    REQUIRE(std::holds_alternative<
            std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(firstClaim));
    const auto& candidates =
        std::get<std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(
            firstClaim);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().start.value == "2099-08-20T10:00:00");
    CHECK_FALSE(candidates.front().recurrenceId.has_value());
    CHECK(candidates.front().displayRecurrenceId ==
          std::optional<javelin::jmap::calendar::LocalDateTime>{{.value = "2099-08-20T10:00:00"}});
    const auto invitationKey = candidates.front().invitationKey;
    const auto duplicateClaim = invitations.claimPendingDispatches();
    REQUIRE(std::holds_alternative<
            std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(
        duplicateClaim));
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(
              duplicateClaim)
              .empty());
    REQUIRE_FALSE(invitations
                      .markDelivered(invitationKey,
                                     QDateTime::fromString(QStringLiteral("2026-08-14T02:00:00Z"),
                                                           Qt::ISODate))
                      .has_value());

    auto accepted = invitation;
    accepted.attendees[1].participationStatus = "accepted";
    auto acceptTransaction = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Optimistic calendar RSVP"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(acceptTransaction));
    auto acceptedProjection =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(acceptTransaction));
    REQUIRE_FALSE(
        calendars.projectEvents(acceptedProjection, "a1", "e2", {accepted}, {}, {}).has_value());
    REQUIRE_FALSE(acceptedProjection.commit().has_value());

    QSqlQuery answered{connection.database()};
    REQUIRE(answered.exec(QStringLiteral(
        "SELECT (SELECT count(*) FROM calendar_pending_invitations WHERE account_id='a1' AND "
        "event_id='invite-1'),status FROM calendar_invitation_outbox WHERE account_id='a1' AND "
        "event_id='invite-1'")));
    REQUIRE(answered.next());
    CHECK(answered.value(0).toInt() == 0);
    CHECK(answered.value(1).toString() == QStringLiteral("resolved"));

    auto rejectTransaction = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Reject calendar RSVP"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(rejectTransaction));
    auto rejectedProjection =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(rejectTransaction));
    REQUIRE_FALSE(
        calendars.projectEvents(rejectedProjection, "a1", "e3", {invitation}, {}, {}).has_value());
    REQUIRE_FALSE(rejectedProjection.commit().has_value());

    QSqlQuery restored{connection.database()};
    REQUIRE(restored.exec(QStringLiteral(
        "SELECT p.self_participant_id,o.status FROM calendar_pending_invitations p JOIN "
        "calendar_invitation_outbox o ON o.account_id=p.account_id AND o.event_id=p.event_id "
        "WHERE p.account_id='a1' AND p.event_id='invite-1'")));
    REQUIRE(restored.next());
    CHECK(restored.value(0).toString() == QStringLiteral("self"));
    CHECK(restored.value(1).toString() == QStringLiteral("resolved"));
    const auto restoredClaim = invitations.claimPendingDispatches();
    REQUIRE(std::holds_alternative<
            std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(restoredClaim));
    CHECK(std::get<std::vector<javelin::jmap::cache::CalendarInvitationDispatchCandidate>>(
              restoredClaim)
              .empty());

    REQUIRE_FALSE(
        invitations
            .reconcile({.accountId = "a1",
                        .notificationState = "n-delivered-requeue",
                        .eventState = "e3",
                        .replaceNotifications = false,
                        .notifications = {},
                        .deletedNotificationIds = {},
                        .events = {invitation},
                        .nonRecurringOccurrences = {},
                        .destroyedEventIds = {},
                        .consideredEventIds = {"invite-1"},
                        .pendingInvitations = {{.eventId = "invite-1",
                                                .selfParticipantId = "self",
                                                .sourceNotificationId = "notification-2",
                                                .enqueueDesktopNotification = true}}})
            .has_value());
    QSqlQuery deliveredRequeue{connection.database()};
    REQUIRE(deliveredRequeue.exec(QStringLiteral(
        "SELECT status,delivered_at FROM calendar_invitation_outbox WHERE account_id='a1' AND "
        "event_id='invite-1' AND recurrence_id=''")));
    REQUIRE(deliveredRequeue.next());
    CHECK(deliveredRequeue.value(0).toString() == QStringLiteral("resolved"));
    CHECK_FALSE(deliveredRequeue.value(1).isNull());

    auto occurrenceInvitations = invitation;
    occurrenceInvitations.attendees[1].participationStatus = "accepted";
    occurrenceInvitations.recurrenceOverrides["2099-08-20T10:00:00"]
        .participantParticipationStatus.insert_or_assign("self", "needs-action");
    occurrenceInvitations.recurrenceOverrides["2099-08-27T10:00:00"]
        .participantParticipationStatus.insert_or_assign("self", "needs-action");
    REQUIRE_FALSE(
        invitations
            .reconcile(
                {.accountId = "a1",
                 .notificationState = "n2",
                 .eventState = "e4",
                 .replaceNotifications = false,
                 .notifications = {},
                 .deletedNotificationIds = {},
                 .events = {occurrenceInvitations},
                 .nonRecurringOccurrences = {},
                 .destroyedEventIds = {},
                 .consideredEventIds = {"invite-1"},
                 .pendingInvitations =
                     {{.eventId = "invite-1",
                       .recurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
                       .selfParticipantId = "self",
                       .displayRecurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
                       .displayStart =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
                       .enqueueDesktopNotification = true},
                      {.eventId = "invite-1",
                       .recurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-27T10:00:00"},
                       .selfParticipantId = "self",
                       .displayRecurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-27T10:00:00"},
                       .displayStart =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-27T10:00:00"},
                       .enqueueDesktopNotification = true}}})
            .has_value());
    QSqlQuery twoOccurrences{connection.database()};
    REQUIRE(twoOccurrences.exec(QStringLiteral(
        "SELECT count(*),count(DISTINCT invitation_key) FROM calendar_invitation_outbox WHERE "
        "account_id='a1' AND event_id='invite-1' AND recurrence_id<>'' AND status='pending'")));
    REQUIRE(twoOccurrences.next());
    CHECK(twoOccurrences.value(0).toInt() == 2);
    CHECK(twoOccurrences.value(1).toInt() == 2);

    auto oneAccepted = javelin::jmap::calendar::setOccurrenceParticipationStatus(
        occurrenceInvitations, {.value = "2099-08-20T10:00:00"}, "self", "accepted");
    auto occurrenceTransaction = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Answer one recurring invitation"));
    REQUIRE(
        std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(occurrenceTransaction));
    auto occurrenceProjection =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(occurrenceTransaction));
    REQUIRE_FALSE(calendars.projectEvents(occurrenceProjection, "a1", "e5", {oneAccepted}, {}, {})
                      .has_value());
    REQUIRE_FALSE(occurrenceProjection.commit().has_value());
    QSqlQuery oneRemaining{connection.database()};
    REQUIRE(oneRemaining.exec(QStringLiteral(
        "SELECT recurrence_id FROM calendar_pending_invitations WHERE account_id='a1' AND "
        "event_id='invite-1' ORDER BY recurrence_id")));
    REQUIRE(oneRemaining.next());
    CHECK(oneRemaining.value(0).toString() == QStringLiteral("2099-08-27T10:00:00"));
    CHECK_FALSE(oneRemaining.next());

    REQUIRE_FALSE(
        invitations
            .reconcile(
                {.accountId = "a1",
                 .notificationState = "n3",
                 .eventState = "e6",
                 .replaceNotifications = false,
                 .notifications = {},
                 .deletedNotificationIds = {},
                 .events = {occurrenceInvitations},
                 .nonRecurringOccurrences = {},
                 .destroyedEventIds = {},
                 .consideredEventIds = {"invite-1"},
                 .pendingInvitations =
                     {{.eventId = "invite-1",
                       .recurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
                       .selfParticipantId = "self",
                       .displayRecurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
                       .displayStart =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-20T10:00:00"},
                       .enqueueDesktopNotification = true},
                      {.eventId = "invite-1",
                       .recurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-27T10:00:00"},
                       .selfParticipantId = "self",
                       .displayRecurrenceId =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-27T10:00:00"},
                       .displayStart =
                           javelin::jmap::calendar::LocalDateTime{.value = "2099-08-27T10:00:00"},
                       .enqueueDesktopNotification = true}}})
            .has_value());
    QSqlQuery reopened{connection.database()};
    REQUIRE(reopened.exec(QStringLiteral(
        "SELECT status,delivered_at,resolved_at FROM calendar_invitation_outbox WHERE "
        "account_id='a1' AND event_id='invite-1' AND recurrence_id='2099-08-20T10:00:00'")));
    REQUIRE(reopened.next());
    CHECK(reopened.value(0).toString() == QStringLiteral("pending"));
    CHECK(reopened.value(1).isNull());
    CHECK(reopened.value(2).isNull());

    auto cancelled = invitation;
    cancelled.status = "cancelled";
    auto cancelTransaction = javelin::jmap::cache::DatabaseTransaction::begin(
        connection, QStringLiteral("Cancel calendar invitation"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(cancelTransaction));
    auto cancelledProjection =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(cancelTransaction));
    REQUIRE_FALSE(
        calendars.projectEvents(cancelledProjection, "a1", "e6", {cancelled}, {}, {}).has_value());
    REQUIRE_FALSE(cancelledProjection.commit().has_value());
    QSqlQuery cancelledRow{connection.database()};
    REQUIRE(cancelledRow.exec(QStringLiteral(
        "SELECT count(*) FROM calendar_pending_invitations WHERE account_id='a1' AND "
        "event_id='invite-1'")));
    REQUIRE(cancelledRow.next());
    CHECK(cancelledRow.value(0).toInt() == 0);
}
