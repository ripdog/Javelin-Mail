#pragma once

#include "jmap/calendar/CalendarTypes.h"

#include <QDateTime>
#include <QTimeZone>

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

    [[nodiscard]] std::optional<CalendarEvent>
    effectiveOccurrenceEvent(const CalendarEvent& baseEvent, const LocalDateTime& recurrenceId);
    [[nodiscard]] CalendarEvent applyOccurrenceEdit(const CalendarEvent& baseEvent,
                                                    const LocalDateTime& recurrenceId,
                                                    const CalendarEvent& editedOccurrence);
    [[nodiscard]] CalendarEvent setOccurrenceParticipationStatus(const CalendarEvent& baseEvent,
                                                                 const LocalDateTime& recurrenceId,
                                                                 std::string_view participantId,
                                                                 std::string participationStatus);
    [[nodiscard]] CalendarEvent excludeOccurrence(const CalendarEvent& baseEvent,
                                                  const LocalDateTime& recurrenceId);
    [[nodiscard]] std::optional<qint64> durationSeconds(const Duration& duration);
    [[nodiscard]] QDateTime alertTrigger(const Alert& alert, const QDateTime& startsAt,
                                         const QDateTime& endsAt,
                                         const QTimeZone& timeZone = QTimeZone::UTC);
    [[nodiscard]] CalendarEvent acknowledgeAlert(CalendarEvent event, Alert alert,
                                                 UtcInstant acknowledgedAt);
} // namespace javelin::jmap::calendar
