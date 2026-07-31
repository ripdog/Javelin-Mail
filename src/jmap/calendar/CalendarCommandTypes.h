#pragma once

#include "jmap/OperationError.h"
#include "jmap/calendar/CalendarReader.h"
#include "jmap/sync/MutationCommitReceipt.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::calendar
{

    struct RefreshedRange
    {
        VisibleInterval interval;
        TimeZoneId displayTimeZone;
        std::size_t accountCount = 0;
        std::size_t eventCount = 0;
    };

    struct CommittedMutation
    {
        std::string accountId;
        std::string newState;
        std::optional<std::string> createdId;
        javelin::jmap::sync::MutationCommitReceipt receipt;
    };

    struct AuthoritativeCalendarEvent
    {
        std::string state;
        std::optional<CalendarEvent> event;
    };

    struct CalendarRangeMaterialization
    {
        VisibleInterval interval;
        TimeZoneId displayTimeZone;
    };

    struct CreateEventCommand
    {
        std::string accountId;
        CalendarEvent event;
        std::optional<std::string> operationGroupId;
        std::optional<std::string> ifInState;
        std::optional<CalendarRangeMaterialization> materialization;
    };

    struct UpdateEventCommand
    {
        std::string accountId;
        CalendarEvent event;
        std::optional<std::string> operationGroupId;
        std::optional<std::string> ifInState;
        std::optional<CalendarRangeMaterialization> materialization;
    };

    struct DeleteEventCommand
    {
        std::string accountId;
        std::string eventId;
        std::vector<std::string> calendarIds;
        std::optional<std::string> operationGroupId;
        std::optional<std::string> ifInState;
    };

    struct CreateCalendarCommand
    {
        std::string accountId;
        std::string name;
        std::optional<std::string> color;
    };

    struct DeleteCalendarCommand
    {
        std::string accountId;
        std::string calendarId;
        bool removeEvents = true;
    };

    using CalendarRefreshResult = std::variant<RefreshedRange, OperationError>;
    using CalendarMutationResult = std::variant<CommittedMutation, OperationError>;
    using CalendarPreferenceResult = std::variant<std::monostate, OperationError>;
    using AuthoritativeCalendarEventResult =
        std::variant<AuthoritativeCalendarEvent, OperationError>;

} // namespace javelin::jmap::calendar
