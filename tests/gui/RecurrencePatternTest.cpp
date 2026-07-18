#include "gui/calendar/RecurrencePattern.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("friendly weekly recurrence preserves selected weekdays and ending count",
          "[gui][calendar][recurrence]")
{
    auto rule = javelin::jmap::calendar::RecurrenceRule{};
    rule.frequency = javelin::jmap::calendar::RecurrenceFrequency::Weekly;
    rule.interval = 2;
    rule.byDay = {{.day = javelin::jmap::calendar::Weekday::Monday, .nthOfPeriod = std::nullopt},
                  {.day = javelin::jmap::calendar::Weekday::Friday, .nthOfPeriod = std::nullopt}};
    rule.count = 12;
    const QDateTime start{QDate{2026, 7, 18}, QTime{9, 30}};

    const auto pattern = javelin::gui::calendar::friendlyRecurrencePattern(rule, start);

    CHECK(pattern.frequency == javelin::gui::calendar::FriendlyRecurrenceFrequency::Week);
    CHECK(pattern.interval == 2);
    CHECK(pattern.weekdays == std::vector{javelin::jmap::calendar::Weekday::Monday,
                                          javelin::jmap::calendar::Weekday::Friday});
    CHECK(pattern.end == javelin::gui::calendar::FriendlyRecurrenceEnd::AfterCount);
    CHECK(pattern.count == 12);
    CHECK_FALSE(pattern.replacesUnsupportedRule);
    CHECK(javelin::gui::calendar::recurrenceRule(pattern, start) == rule);
}

TEST_CASE("friendly monthly recurrence offers date and ordinal weekday forms",
          "[gui][calendar][recurrence]")
{
    const QDateTime start{QDate{2026, 7, 18}, QTime{9, 30}};
    CHECK(javelin::gui::calendar::ordinalWeekday(start.date()) == 3);

    auto dateRule = javelin::jmap::calendar::RecurrenceRule{};
    dateRule.frequency = javelin::jmap::calendar::RecurrenceFrequency::Monthly;
    dateRule.byMonthDay = {18};
    const auto datePattern = javelin::gui::calendar::friendlyRecurrencePattern(dateRule, start);
    CHECK(datePattern.monthlyMode == javelin::gui::calendar::FriendlyMonthlyMode::DayOfMonth);
    CHECK_FALSE(datePattern.replacesUnsupportedRule);

    auto ordinalRule = javelin::jmap::calendar::RecurrenceRule{};
    ordinalRule.frequency = javelin::jmap::calendar::RecurrenceFrequency::Monthly;
    ordinalRule.byDay = {{.day = javelin::jmap::calendar::Weekday::Saturday, .nthOfPeriod = 3}};
    const auto ordinalPattern =
        javelin::gui::calendar::friendlyRecurrencePattern(ordinalRule, start);
    CHECK(ordinalPattern.monthlyMode ==
          javelin::gui::calendar::FriendlyMonthlyMode::OrdinalWeekday);
    CHECK_FALSE(ordinalPattern.replacesUnsupportedRule);
    CHECK(javelin::gui::calendar::recurrenceRule(ordinalPattern, start) == ordinalRule);
}

TEST_CASE("friendly recurrence preserves end dates at the event local time",
          "[gui][calendar][recurrence]")
{
    const QDateTime start{QDate{2026, 7, 18}, QTime{9, 30}};
    javelin::gui::calendar::FriendlyRecurrencePattern pattern;
    pattern.frequency = javelin::gui::calendar::FriendlyRecurrenceFrequency::Year;
    pattern.end = javelin::gui::calendar::FriendlyRecurrenceEnd::OnDate;
    pattern.untilDate = QDate{2029, 7, 18};

    const auto rule = javelin::gui::calendar::recurrenceRule(pattern, start);

    CHECK(rule.frequency == javelin::jmap::calendar::RecurrenceFrequency::Yearly);
    CHECK(rule.until ==
          std::optional{javelin::jmap::calendar::LocalDateTime{.value = "2029-07-18T09:30:00"}});
    const auto hydrated = javelin::gui::calendar::friendlyRecurrencePattern(rule, start);
    CHECK(hydrated.end == javelin::gui::calendar::FriendlyRecurrenceEnd::OnDate);
    CHECK(hydrated.untilDate == std::optional{QDate{2029, 7, 18}});
}

TEST_CASE("uncommon recurrence selectors are identified before replacement",
          "[gui][calendar][recurrence]")
{
    auto rule = javelin::jmap::calendar::RecurrenceRule{};
    rule.frequency = javelin::jmap::calendar::RecurrenceFrequency::Hourly;
    rule.byDay = {{.day = javelin::jmap::calendar::Weekday::Monday, .nthOfPeriod = std::nullopt}};
    rule.bySetPosition = {-1};

    const auto pattern = javelin::gui::calendar::friendlyRecurrencePattern(
        rule, QDateTime{QDate{2026, 7, 18}, QTime{9, 30}});

    CHECK(pattern.replacesUnsupportedRule);
    CHECK(pattern.frequency == javelin::gui::calendar::FriendlyRecurrenceFrequency::Day);
}
