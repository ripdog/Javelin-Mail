#include "gui/calendar/CalendarNotificationEditor.h"
#include "gui/calendar/EventDialog.h"

#include <QDateTimeEdit>
#include <QPushButton>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("calendar notification editor exposes relative reminders only",
          "[gui][calendar][notifications]")
{
    javelin::gui::calendar::CalendarNotificationEditor editor{false};
    const std::unordered_map<std::string, javelin::jmap::calendar::Alert> alerts{
        {"relative",
         {.id = "relative",
          .action = "display",
          .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
          .relativeTo = "start",
          .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
          .when = std::nullopt,
          .acknowledged = std::nullopt}},
        {"absolute",
         {.id = "absolute",
          .action = "display",
          .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Absolute,
          .relativeTo = "start",
          .offset = std::nullopt,
          .when = javelin::jmap::calendar::UtcInstant{.value = "2026-08-21T10:00:00Z"},
          .acknowledged = std::nullopt}},
    };

    editor.setAlerts(alerts);

    CHECK(editor.findChildren<QDateTimeEdit*>().empty());
    const auto displayed = editor.displayAlerts();
    REQUIRE(displayed.size() == 1);
    REQUIRE(displayed.contains("relative"));
    CHECK(displayed.at("relative").triggerKind ==
          javelin::jmap::calendar::AlertTriggerKind::Offset);
}

TEST_CASE("calendar notification editor preserves untouched offset representation",
          "[gui][calendar][notifications]")
{
    javelin::gui::calendar::CalendarNotificationEditor editor{false};
    const std::unordered_map<std::string, javelin::jmap::calendar::Alert> alerts{
        {"relative",
         {.id = "relative",
          .action = "display",
          .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
          .relativeTo = "end",
          .offset = javelin::jmap::calendar::Duration{.value = "-P2DT3H4M5S"},
          .when = std::nullopt,
          .acknowledged = javelin::jmap::calendar::UtcInstant{.value = "2026-08-21T00:00:00Z"}}},
    };

    editor.setAlerts(alerts);

    const auto displayed = editor.displayAlerts();
    REQUIRE(displayed.contains("relative"));
    CHECK(displayed.at("relative").relativeTo == "end");
    REQUIRE(displayed.at("relative").offset.has_value());
    CHECK(displayed.at("relative").offset->value == "-P2DT3H4M5S");
    CHECK(displayed.at("relative").acknowledged == alerts.at("relative").acknowledged);
}

TEST_CASE("event notification edits preserve legacy absolute reminders",
          "[gui][calendar][notifications]")
{
    const javelin::jmap::calendar::Calendar calendar{
        .accountId = "account",
        .id = "calendar",
        .name = "Calendar",
        .description = std::nullopt,
        .color = std::nullopt,
        .sortOrder = 0,
        .isSubscribed = true,
        .isVisible = true,
        .isDefault = true,
        .timeZone = std::nullopt,
        .defaultAlertsWithTime = {},
        .defaultAlertsWithoutTime = {},
        .myRights = {.mayWriteAll = true},
    };
    javelin::jmap::calendar::CalendarEvent event;
    event.accountId = "account";
    event.id = "event";
    event.uid = "uid";
    event.calendarIds = {{"calendar", true}};
    event.title = "Event";
    event.start = {.value = "2026-08-21T12:00:00"};
    event.duration = {.value = "PT1H"};
    event.timeZone = javelin::jmap::calendar::TimeZoneId{.value = "Pacific/Auckland"};
    event.alerts = {
        {"relative",
         {.id = "relative",
          .action = "display",
          .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Offset,
          .relativeTo = "start",
          .offset = javelin::jmap::calendar::Duration{.value = "-PT10M"},
          .when = std::nullopt,
          .acknowledged = std::nullopt}},
        {"absolute",
         {.id = "absolute",
          .action = "display",
          .triggerKind = javelin::jmap::calendar::AlertTriggerKind::Absolute,
          .relativeTo = "start",
          .offset = std::nullopt,
          .when = javelin::jmap::calendar::UtcInstant{.value = "2026-08-21T00:00:00Z"},
          .acknowledged = std::nullopt}},
    };

    javelin::gui::calendar::EventDialog dialog{{calendar}};
    dialog.setEvent(event);
    auto* add = dialog.findChild<QPushButton*>(QStringLiteral("addNotification"));
    REQUIRE(add != nullptr);
    add->click();

    const auto edited = dialog.eventDocument();
    REQUIRE(edited.alerts.contains("absolute"));
    CHECK(edited.alerts.at("absolute").triggerKind ==
          javelin::jmap::calendar::AlertTriggerKind::Absolute);
    CHECK(edited.alerts.at("absolute").when == event.alerts.at("absolute").when);
}
