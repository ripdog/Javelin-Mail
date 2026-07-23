#pragma once

#include "gui/calendar/MonthCalendarWidget.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/calendar/CalendarTypes.h"

#include <QColor>

#include <optional>
#include <vector>

namespace javelin::gui::calendar
{
    struct CalendarAccountPresentation
    {
        std::vector<CalendarDisplay> calendars;
        std::vector<MonthEvent> events;
    };

    [[nodiscard]] CalendarAccountPresentation buildCalendarAccountPresentation(
        const javelin::jmap::cache::CalendarAccount& account,
        const std::vector<javelin::jmap::calendar::Calendar>& calendars,
        const std::optional<javelin::jmap::cache::CalendarWindow>& window,
        const QColor& fallbackColor);
} // namespace javelin::gui::calendar
