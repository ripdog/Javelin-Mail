#pragma once

#include "gui/calendar/MonthCalendarWidget.h"
#include "jmap/calendar/CalendarReader.h"
#include "jmap/calendar/CalendarTypes.h"
#include "protocol/SettingsContract.h"

#include <QColor>
#include <QString>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::gui::calendar
{
    struct DayAgendaEvent;

    struct CalendarAccountPresentation
    {
        std::vector<CalendarDisplay> calendars;
        std::vector<MonthEvent> events;
    };

    struct NewEventCalendarCandidate
    {
        std::string ownerAccountId;
        std::string accountId;
        std::string calendarId;
        bool writable = false;
        bool serverDefault = false;
    };

    [[nodiscard]] std::optional<std::size_t> preferredNewEventCalendarIndex(
        std::span<const NewEventCalendarCandidate> candidates,
        const javelin::protocol::CalendarDefaultDestination& configuredDestination);

    [[nodiscard]] DayAgendaEvent dayAgendaEventFromMonthEvent(const MonthEvent& event);

    [[nodiscard]] QColor automaticCalendarColor(std::string_view ownerAccountId,
                                                std::string_view accountId,
                                                std::string_view calendarId,
                                                const QColor& surfaceColor);

    [[nodiscard]] CalendarAccountPresentation buildCalendarAccountPresentation(
        const javelin::jmap::cache::CalendarAccount& account,
        const std::vector<javelin::jmap::calendar::Calendar>& calendars,
        const std::optional<javelin::jmap::cache::CalendarWindow>& window,
        const QColor& surfaceColor);

    [[nodiscard]] QString
    eventConfirmationDetails(const javelin::jmap::calendar::CalendarEvent& event);
} // namespace javelin::gui::calendar
