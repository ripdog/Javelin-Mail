#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::jmap::calendar
{
    [[nodiscard]] std::optional<std::size_t>
    participantIndexForAddress(const CalendarEvent& event, std::string_view calendarAddress);
    [[nodiscard]] bool eventOwnedByAddress(const CalendarEvent& event,
                                           std::string_view calendarAddress);
    [[nodiscard]] bool eventHasOwner(const CalendarEvent& event);
    [[nodiscard]] bool eventInvitesAddress(const CalendarEvent& event,
                                           std::string_view calendarAddress);
    [[nodiscard]] bool eventEditableWithRights(const CalendarEvent& event,
                                               const CalendarRights& rights,
                                               std::string_view calendarAddress);

    [[nodiscard]] std::vector<std::string>
    editableAttendeeAddresses(const std::vector<Attendee>& attendees);

    [[nodiscard]] std::vector<Attendee>
    reconcileEditableAttendees(const std::vector<Attendee>& existing,
                               std::span<const std::string> requestedAddresses);

    [[nodiscard]] CalendarEvent applyOccurrenceEdit(const CalendarEvent& baseEvent,
                                                    const LocalDateTime& recurrenceId,
                                                    const CalendarEvent& editedOccurrence);
    [[nodiscard]] CalendarEvent excludeOccurrence(const CalendarEvent& baseEvent,
                                                  const LocalDateTime& recurrenceId);
    [[nodiscard]] CalendarEvent acknowledgeAlert(CalendarEvent event, Alert alert,
                                                 UtcInstant acknowledgedAt);
} // namespace javelin::jmap::calendar
