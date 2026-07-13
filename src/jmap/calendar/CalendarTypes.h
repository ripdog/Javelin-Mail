#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::calendar
{
    struct LocalDate
    {
        std::string value;
        auto operator<=>(const LocalDate&) const = default;
    };

    struct LocalDateTime
    {
        std::string value;
        auto operator<=>(const LocalDateTime&) const = default;
    };

    struct UtcInstant
    {
        std::string value;
        auto operator<=>(const UtcInstant&) const = default;
    };

    struct Duration
    {
        std::string value;
        auto operator<=>(const Duration&) const = default;
    };

    struct TimeZoneId
    {
        std::string value;
        auto operator<=>(const TimeZoneId&) const = default;
    };

    struct CalendarRights
    {
        bool mayReadFreeBusy = false;
        bool mayReadItems = false;
        bool mayWriteAll = false;
        bool mayWriteOwn = false;
        bool mayUpdatePrivate = false;
        bool mayRSVP = false;
        bool mayShare = false;
        bool mayDelete = false;
    };

    struct Calendar
    {
        std::string accountId;
        std::string id;
        std::string name;
        std::optional<std::string> description;
        std::optional<std::string> color;
        std::uint32_t sortOrder = 0;
        bool isSubscribed = false;
        bool isVisible = true;
        bool isDefault = false;
        std::optional<TimeZoneId> timeZone;
        CalendarRights myRights;
    };

    enum class RecurrenceFrequency
    {
        Yearly,
        Monthly,
        Weekly,
        Daily,
        Hourly,
        Minutely,
        Secondly,
    };

    struct RecurrenceRule
    {
        RecurrenceFrequency frequency = RecurrenceFrequency::Daily;
        std::uint32_t interval = 1;
        std::optional<std::uint32_t> count;
        std::optional<LocalDateTime> until;
    };

    struct Attendee
    {
        std::string id;
        std::string name;
        std::optional<std::string> email;
        std::string calendarAddress;
        std::string participationStatus;
        bool isOwner = false;
        bool isAttendee = true;
        std::uint32_t scheduleSequence = 0;
        std::optional<UtcInstant> scheduleUpdated;
    };

    struct RecurrenceOverride
    {
        bool excluded = false;
        std::optional<LocalDateTime> start;
        std::optional<Duration> duration;
        std::optional<std::string> title;
    };

    struct CalendarEvent
    {
        std::string accountId;
        std::string id;
        std::string uid;
        std::unordered_map<std::string, bool> calendarIds;
        std::string title;
        std::optional<std::string> description;
        std::optional<std::string> location;
        LocalDateTime start;
        Duration duration;
        TimeZoneId timeZone{.value = "Etc/UTC"};
        bool showWithoutTime = false;
        bool isDraft = false;
        bool isOrigin = false;
        std::optional<UtcInstant> utcStart;
        std::optional<UtcInstant> utcEnd;
        std::optional<RecurrenceRule> recurrenceRule;
        std::unordered_map<std::string, RecurrenceOverride> recurrenceOverrides;
        std::vector<Attendee> attendees;
    };

    struct Occurrence
    {
        std::string accountId;
        std::string id;
        std::string eventId;
        std::optional<LocalDateTime> recurrenceId;
        LocalDateTime localStart;
        LocalDateTime localEnd;
        std::optional<UtcInstant> utcStart;
        std::optional<UtcInstant> utcEnd;
        bool allDay = false;
    };

} // namespace javelin::jmap::calendar
