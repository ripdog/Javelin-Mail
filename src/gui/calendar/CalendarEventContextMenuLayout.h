#pragma once

#include <QString>

#include <vector>

namespace javelin::gui::calendar
{
    struct CalendarEventContextMenuAvailability
    {
        bool editable = false;
        bool duplicable = false;
        bool movable = false;
        bool rsvp = false;
    };

    [[nodiscard]] const QString& calendarEventContextMenuSeparatorId();
    [[nodiscard]] const std::vector<QString>& supportedCalendarEventContextMenuActionIds();
    [[nodiscard]] const std::vector<QString>& defaultCalendarEventContextMenuLayout();
    [[nodiscard]] std::vector<QString>
    normalizeCalendarEventContextMenuLayout(const std::vector<QString>& layout);
    [[nodiscard]] std::vector<QString>
    effectiveCalendarEventContextMenuLayout(const std::vector<QString>& configuredLayout);
    [[nodiscard]] std::vector<QString>
    calendarEventContextMenuOverrideForLayout(const std::vector<QString>& layout);
    [[nodiscard]] std::vector<QString>
    visibleCalendarEventContextMenuLayout(const std::vector<QString>& configuredLayout,
                                          const CalendarEventContextMenuAvailability& availability);
} // namespace javelin::gui::calendar
