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

    enum class AlertTriggerKind
    {
        Offset,
        Absolute,
    };

    struct Alert
    {
        std::string id;
        std::string action;
        AlertTriggerKind triggerKind = AlertTriggerKind::Offset;
        std::string relativeTo = "start";
        std::optional<Duration> offset;
        std::optional<UtcInstant> when;
        std::optional<UtcInstant> acknowledged;

        bool operator==(const Alert&) const = default;
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
        std::unordered_map<std::string, Alert> defaultAlertsWithTime;
        std::unordered_map<std::string, Alert> defaultAlertsWithoutTime;
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

    enum class RecurrenceSkip
    {
        Omit,
        Backward,
        Forward,
    };

    enum class Weekday
    {
        Monday,
        Tuesday,
        Wednesday,
        Thursday,
        Friday,
        Saturday,
        Sunday,
    };

    struct RecurrenceDay
    {
        Weekday day = Weekday::Monday;
        std::optional<std::int32_t> nthOfPeriod;

        auto operator<=>(const RecurrenceDay&) const = default;
    };

    struct RecurrenceRule
    {
        RecurrenceFrequency frequency = RecurrenceFrequency::Daily;
        std::uint32_t interval = 1;
        std::optional<std::string> rscale;
        std::optional<RecurrenceSkip> skip;
        std::optional<Weekday> firstDayOfWeek;
        std::vector<RecurrenceDay> byDay;
        std::vector<std::int32_t> byMonthDay;
        std::vector<std::string> byMonth;
        std::vector<std::int32_t> byYearDay;
        std::vector<std::int32_t> byWeekNo;
        std::vector<std::uint32_t> byHour;
        std::vector<std::uint32_t> byMinute;
        std::vector<std::uint32_t> bySecond;
        std::vector<std::int32_t> bySetPosition;
        std::optional<std::uint32_t> count;
        std::optional<LocalDateTime> until;

        auto operator<=>(const RecurrenceRule&) const = default;
    };

    struct ParticipantIdentity
    {
        std::string id;
        std::string name;
        std::string calendarAddress;
        bool isDefault = false;

        bool operator==(const ParticipantIdentity&) const = default;
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
        std::unordered_map<std::string, bool> roles;
        bool expectReply = false;
        std::uint32_t scheduleSequence = 0;
        std::optional<UtcInstant> scheduleUpdated;

        bool operator==(const Attendee&) const = default;
    };

    struct RecurrenceOverride
    {
        bool excluded = false;
        std::optional<LocalDateTime> start;
        std::optional<Duration> duration;
        std::optional<std::string> title;

        bool operator==(const RecurrenceOverride&) const = default;
    };

    struct CalendarEvent
    {
        std::string accountId;
        std::string id;
        std::optional<std::string> baseEventId;
        std::optional<LocalDateTime> recurrenceId;
        std::string uid;
        std::unordered_map<std::string, bool> calendarIds;
        std::string title;
        std::optional<std::string> description;
        std::optional<std::string> location;
        LocalDateTime start;
        Duration duration;
        std::optional<TimeZoneId> timeZone;
        bool showWithoutTime = false;
        bool isDraft = false;
        bool isOrigin = false;
        std::optional<std::string> organizerCalendarAddress = std::nullopt;
        bool useDefaultAlerts = false;
        std::unordered_map<std::string, Alert> alerts;
        std::optional<UtcInstant> utcStart;
        std::optional<UtcInstant> utcEnd;
        std::optional<RecurrenceRule> recurrenceRule;
        std::unordered_map<std::string, RecurrenceOverride> recurrenceOverrides;
        std::vector<Attendee> attendees;

        bool operator==(const CalendarEvent&) const = default;
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
