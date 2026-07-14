#include "jmap/cache/CalendarRepository.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

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
    CHECK(std::get<std::optional<std::string>>(calendarState) == std::optional<std::string>{"c1"});
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
