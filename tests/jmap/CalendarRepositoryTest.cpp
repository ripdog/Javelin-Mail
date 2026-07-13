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

    QSqlQuery count{connection.database()};
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_occurrences")));
    REQUIRE(count.next());
    CHECK(count.value(0).toInt() == 1);
}
