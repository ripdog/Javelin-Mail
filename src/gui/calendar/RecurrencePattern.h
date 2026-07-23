#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDate>
#include <QDateTime>

#include <cstdint>
#include <optional>
#include <vector>

namespace javelin::gui::calendar
{
    enum class FriendlyRecurrenceFrequency
    {
        Day,
        Week,
        Month,
        Year,
    };

    enum class FriendlyMonthlyMode
    {
        DayOfMonth,
        OrdinalWeekday,
    };

    enum class FriendlyRecurrenceEnd
    {
        Never,
        OnDate,
        AfterCount,
    };

    struct FriendlyRecurrencePattern
    {
        FriendlyRecurrenceFrequency frequency = FriendlyRecurrenceFrequency::Day;
        std::uint32_t interval = 1;
        std::vector<javelin::jmap::calendar::Weekday> weekdays;
        FriendlyMonthlyMode monthlyMode = FriendlyMonthlyMode::DayOfMonth;
        FriendlyRecurrenceEnd end = FriendlyRecurrenceEnd::Never;
        std::optional<QDate> untilDate;
        std::uint32_t count = 1;
        std::optional<javelin::jmap::calendar::Weekday> firstDayOfWeek;
        bool replacesUnsupportedRule = false;
    };

    [[nodiscard]] FriendlyRecurrencePattern
    friendlyRecurrencePattern(const javelin::jmap::calendar::RecurrenceRule& rule,
                              const QDateTime& eventStart);
    [[nodiscard]] javelin::jmap::calendar::RecurrenceRule
    recurrenceRule(const FriendlyRecurrencePattern& pattern, const QDateTime& eventStart);
    [[nodiscard]] javelin::jmap::calendar::Weekday weekday(Qt::DayOfWeek day);
    [[nodiscard]] int ordinalWeekday(const QDate& date);
} // namespace javelin::gui::calendar
