#include "jmap/cache/CalendarNotificationRepository.h"
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
            static char name[] = "calendar-notification-repository-test";
            static char* argv[] = {name, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };
} // namespace

TEST_CASE("calendar notification scans expose the next exact trigger and sync context",
          "[jmap][calendar][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = QStringLiteral("calendar-notification-next-trigger"),
         .databasePath = directory.filePath(QStringLiteral("cache.sqlite3"))});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    REQUIRE(QSqlQuery{connection.database()}.exec(QStringLiteral(
        "INSERT INTO accounts "
        "(account_id,email_address,session_url,is_primary,owner_account_id) VALUES "
        "('calendar-account','alice@example.test','https://example.test/jmap',1,'connection-1')")));

    javelin::jmap::cache::CalendarRepository calendars{connection};
    const javelin::jmap::calendar::Calendar calendar{
        .accountId = "calendar-account",
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
    REQUIRE_FALSE(
        calendars.replaceCalendars("calendar-account", "calendar-state", {calendar}).has_value());

    const javelin::jmap::calendar::Alert alert{
        .id = "ten-minutes",
        .action = "display",
        .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
        .relativeTo = "start",
        .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
        .when = std::nullopt,
        .acknowledged = std::nullopt};
    const javelin::jmap::calendar::CalendarEvent event{
        .accountId = "calendar-account",
        .id = "event-1",
        .baseEventId = std::nullopt,
        .recurrenceId = std::nullopt,
        .uid = "uid-event-1",
        .calendarIds = {{"work", true}},
        .title = "Stand-up",
        .description = std::nullopt,
        .location = std::nullopt,
        .start = {.value = "2026-07-28T10:00:00"},
        .duration = {.value = "PT30M"},
        .timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Etc/UTC"},
        .showWithoutTime = false,
        .isDraft = false,
        .isOrigin = true,
        .useDefaultAlerts = false,
        .alerts = {{"ten-minutes", alert}},
        .utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-07-28T10:00:00Z"},
        .utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-07-28T10:30:00Z"},
        .recurrenceRule = std::nullopt,
        .recurrenceOverrides = {},
        .attendees = {}};
    const javelin::jmap::calendar::Occurrence occurrence{
        .accountId = "calendar-account",
        .id = "event-1",
        .eventId = "event-1",
        .recurrenceId = std::nullopt,
        .localStart = {.value = "2026-07-28T10:00:00"},
        .localEnd = {.value = "2026-07-28T10:30:00"},
        .utcStart = javelin::jmap::calendar::UtcInstant{.value = "2026-07-28T10:00:00Z"},
        .utcEnd = javelin::jmap::calendar::UtcInstant{.value = "2026-07-28T10:30:00Z"},
        .allDay = false};
    REQUIRE_FALSE(calendars
                      .reconcileReminderHorizon({.accountId = "calendar-account",
                                                 .start = {.value = "2026-07-01T00:00:00"},
                                                 .end = {.value = "2026-08-01T00:00:00"},
                                                 .displayTimeZone = {.value = "Etc/UTC"},
                                                 .eventState = "event-state",
                                                 .events = {event},
                                                 .occurrences = {occurrence}})
                      .has_value());

    javelin::jmap::cache::CalendarNotificationRepository notifications{connection};
    const auto before = QDateTime::fromString(QStringLiteral("2026-07-28T09:40:00Z"), Qt::ISODate);
    const auto early = notifications.claimDue(before);
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(
            early));
    CHECK(
        std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(early).empty());
    REQUIRE(notifications.nextTrigger().has_value());
    CHECK(*notifications.nextTrigger() ==
          QDateTime::fromString(QStringLiteral("2026-07-28T09:50:00Z"), Qt::ISODate));

    const auto trigger = *notifications.nextTrigger();
    const auto due = notifications.claimDue(trigger);
    const auto& candidates =
        std::get<std::vector<javelin::jmap::cache::CalendarNotificationCandidate>>(due);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().ownerAccountId == "connection-1");
    CHECK(candidates.front().alert == alert);
}
