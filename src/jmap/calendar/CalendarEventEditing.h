#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <span>
#include <string>
#include <vector>

namespace javelin::jmap::calendar
{
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
