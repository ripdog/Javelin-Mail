#include "gui/calendar/CalendarEventContextMenuLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace javelin::gui::calendar;

TEST_CASE("calendar event context menu defaults isolate deletion")
{
    const auto& layout = defaultCalendarEventContextMenuLayout();
    REQUIRE(layout.size() == 12);
    CHECK(layout.front() == QStringLiteral("calendar_event_edit"));
    CHECK(layout[2] == calendarEventContextMenuSeparatorId());
    CHECK(layout[4] == calendarEventContextMenuSeparatorId());
    CHECK(layout[8] == calendarEventContextMenuSeparatorId());
    CHECK(layout[10] == calendarEventContextMenuSeparatorId());
    CHECK(layout.back() == QStringLiteral("calendar_event_delete"));
}

TEST_CASE("calendar event context menu normalization ignores invalid and duplicate actions")
{
    const auto normalized = normalizeCalendarEventContextMenuLayout({
        calendarEventContextMenuSeparatorId(),
        QStringLiteral("calendar_event_edit"),
        calendarEventContextMenuSeparatorId(),
        calendarEventContextMenuSeparatorId(),
        QStringLiteral("unknown_action"),
        QStringLiteral("calendar_event_edit"),
        QStringLiteral("calendar_event_delete"),
        calendarEventContextMenuSeparatorId(),
    });

    CHECK(normalized == std::vector<QString>{QStringLiteral("calendar_event_edit"),
                                             calendarEventContextMenuSeparatorId(),
                                             QStringLiteral("calendar_event_delete")});
}

TEST_CASE("empty calendar event context menu override selects current defaults")
{
    CHECK(effectiveCalendarEventContextMenuLayout({}) == defaultCalendarEventContextMenuLayout());
    CHECK(
        effectiveCalendarEventContextMenuLayout({QStringLiteral("calendar_event_copy_details")}) ==
        std::vector<QString>{QStringLiteral("calendar_event_copy_details")});
}

TEST_CASE("saving the default calendar event context menu clears its override")
{
    CHECK(
        calendarEventContextMenuOverrideForLayout(defaultCalendarEventContextMenuLayout()).empty());
}

TEST_CASE("calendar event context filtering distinguishes owned invitations and read-only events")
{
    const auto owned = visibleCalendarEventContextMenuLayout(
        {}, {.editable = true, .duplicable = true, .movable = true, .rsvp = false});
    CHECK(std::ranges::contains(owned, QStringLiteral("calendar_event_edit")));
    CHECK(std::ranges::contains(owned, QStringLiteral("calendar_event_move")));
    CHECK(std::ranges::contains(owned, QStringLiteral("calendar_event_delete")));
    CHECK_FALSE(std::ranges::contains(owned, QStringLiteral("calendar_event_accept")));

    const auto invitation = visibleCalendarEventContextMenuLayout(
        {}, {.editable = false, .duplicable = true, .movable = false, .rsvp = true});
    CHECK_FALSE(std::ranges::contains(invitation, QStringLiteral("calendar_event_edit")));
    CHECK(std::ranges::contains(invitation, QStringLiteral("calendar_event_accept")));
    CHECK_FALSE(std::ranges::contains(invitation, QStringLiteral("calendar_event_delete")));

    const auto readOnly = visibleCalendarEventContextMenuLayout(
        {}, {.editable = false, .duplicable = false, .movable = false, .rsvp = false});
    CHECK(readOnly == std::vector<QString>{QStringLiteral("calendar_event_copy_details")});
}
