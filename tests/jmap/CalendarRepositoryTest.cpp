#include "jmap/cache/CalendarRepository.h"
#include "jmap/api/CalendarMethods.h"
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
    const javelin::jmap::calendar::Calendar calendar{
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
    REQUIRE_FALSE(repository.setDefaultCalendar("a1", "work").has_value());
    auto serverChangedCalendar = calendar;
    serverChangedCalendar.isDefault = false;
    REQUIRE_FALSE(
        repository.replaceCalendars("a1", "c1-refreshed", {serverChangedCalendar}).has_value());
    hiddenCalendars = repository.listCalendars("a1");
    CHECK_FALSE(std::get<std::vector<javelin::jmap::calendar::Calendar>>(hiddenCalendars)
                    .front()
                    .isVisible);
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
          std::optional<std::string>{"c1-refreshed"});
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
