#include "jmap/api/CalendarMethods.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <utility>
#include <variant>

namespace javelin::jmap::api::detail
{
    struct RawCalendarRights
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

    struct RawTrigger
    {
        std::string type;
        std::string relativeTo = "start";
        std::optional<std::string> offset;
        std::optional<std::string> when;

        bool operator==(const RawTrigger&) const = default;
    };

    struct RawAlert
    {
        std::string type;
        std::string action;
        RawTrigger trigger;
        std::optional<std::string> acknowledged;

        bool operator==(const RawAlert&) const = default;
    };

    struct RawCalendar
    {
        std::string id;
        std::string name;
        std::optional<std::string> description;
        std::optional<std::string> color;
        std::uint32_t sortOrder = 0;
        bool isSubscribed = false;
        bool isVisible = true;
        bool isDefault = false;
        std::optional<std::string> timeZone;
        std::unordered_map<std::string, RawAlert> defaultAlertsWithTime;
        std::unordered_map<std::string, RawAlert> defaultAlertsWithoutTime;
        RawCalendarRights myRights;
    };

    struct RawCalendarGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<RawCalendar> list;
        std::vector<std::string> notFound;
    };

    struct RawQueryFilter
    {
        std::optional<std::string> inCalendar;
        std::optional<std::string> after;
        std::optional<std::string> before;
        std::optional<std::string> text;
        std::optional<std::string> uid;
    };

    struct RawQueryRequest
    {
        std::string accountId;
        RawQueryFilter filter;
        bool expandRecurrences = true;
        std::string timeZone;
        std::uint64_t position = 0;
        std::optional<std::uint64_t> limit;
        bool calculateTotal = true;
    };

    struct RawQueryResponse
    {
        std::string accountId;
        std::string queryState;
        bool canCalculateChanges = false;
        std::uint64_t position = 0;
        std::vector<std::string> ids;
        std::optional<std::uint64_t> total;
        std::optional<std::uint64_t> limit;
    };

    struct RawResultReference
    {
        std::string resultOf;
        std::string name;
        std::string path;
    };

    struct RawEventGetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<RawResultReference> idsReference;
        std::optional<std::vector<std::string>> properties;
        std::optional<std::string> recurrenceOverridesBefore;
        std::optional<std::string> recurrenceOverridesAfter;
        bool reduceParticipants = false;
        std::string timeZone;
    };

    struct RawRecurrenceDay
    {
        std::string type;
        std::string day;
        std::optional<std::int32_t> nthOfPeriod;

        bool operator==(const RawRecurrenceDay&) const = default;
    };

    struct RawRecurrenceRule
    {
        std::string type;
        std::string frequency;
        std::uint32_t interval = 1;
        std::optional<std::string> rscale;
        std::optional<std::string> skip;
        std::optional<std::string> firstDayOfWeek;
        std::optional<std::vector<RawRecurrenceDay>> byDay;
        std::optional<std::vector<std::int32_t>> byMonthDay;
        std::optional<std::vector<std::string>> byMonth;
        std::optional<std::vector<std::int32_t>> byYearDay;
        std::optional<std::vector<std::int32_t>> byWeekNo;
        std::optional<std::vector<std::uint32_t>> byHour;
        std::optional<std::vector<std::uint32_t>> byMinute;
        std::optional<std::vector<std::uint32_t>> bySecond;
        std::optional<std::vector<std::int32_t>> bySetPosition;
        std::optional<std::uint32_t> count;
        std::optional<std::string> until;

        bool operator==(const RawRecurrenceRule&) const = default;
    };

    struct RawOverride
    {
        bool excluded = false;
        std::optional<std::string> start;
        std::optional<std::string> duration;
        std::optional<std::string> title;

        bool operator==(const RawOverride&) const = default;
    };

    struct RawParticipant
    {
        std::string type;
        std::string name;
        std::optional<std::string> email;
        std::string calendarAddress;
        std::string participationStatus;
        std::unordered_map<std::string, bool> roles;
        std::uint32_t scheduleSequence = 0;
        std::optional<std::string> scheduleUpdated;

        bool operator==(const RawParticipant&) const = default;
    };

    struct RawLocation
    {
        std::string type;
        std::string name;

        bool operator==(const RawLocation&) const = default;
    };

    struct RawEvent
    {
        std::string type;
        std::string id;
        std::optional<std::string> baseEventId;
        std::optional<std::string> recurrenceId;
        std::string uid;
        std::unordered_map<std::string, bool> calendarIds;
        std::string title;
        std::optional<std::string> description;
        std::unordered_map<std::string, RawLocation> locations;
        std::string start;
        std::string duration;
        std::optional<std::string> timeZone;
        bool showWithoutTime = false;
        bool isDraft = false;
        bool isOrigin = false;
        bool useDefaultAlerts = false;
        std::unordered_map<std::string, RawAlert> alerts;
        std::optional<std::string> utcStart;
        std::optional<std::string> utcEnd;
        std::optional<RawRecurrenceRule> recurrenceRule;
        std::unordered_map<std::string, RawOverride> recurrenceOverrides;
        std::unordered_map<std::string, RawParticipant> participants;
    };

    struct RawEventWrite
    {
        std::string type;
        std::optional<std::string> uid;
        std::unordered_map<std::string, bool> calendarIds;
        std::string title;
        std::optional<std::string> description;
        std::unordered_map<std::string, RawLocation> locations;
        std::string start;
        std::string duration;
        std::optional<std::string> timeZone;
        bool showWithoutTime = false;
        bool isDraft = false;
        bool useDefaultAlerts = false;
        std::unordered_map<std::string, RawAlert> alerts;
        std::optional<RawRecurrenceRule> recurrenceRule;
        std::unordered_map<std::string, RawOverride> recurrenceOverrides;
        std::unordered_map<std::string, RawParticipant> participants;

        bool operator==(const RawEventWrite&) const = default;
    };

    struct RawEventPatch
    {
        std::optional<std::unordered_map<std::string, bool>> calendarIds;
        std::optional<std::string> title;
        std::optional<std::variant<std::nullptr_t, std::string>> description;
        std::optional<std::unordered_map<std::string, RawLocation>> locations;
        std::optional<std::string> start;
        std::optional<std::string> duration;
        std::optional<std::variant<std::nullptr_t, std::string>> timeZone;
        std::optional<bool> showWithoutTime;
        std::optional<bool> isDraft;
        std::optional<bool> useDefaultAlerts;
        std::optional<std::unordered_map<std::string, RawAlert>> alerts;
        std::optional<std::variant<std::nullptr_t, RawRecurrenceRule>> recurrenceRule;
        std::optional<std::unordered_map<std::string, RawOverride>> recurrenceOverrides;
        std::optional<std::unordered_map<std::string, RawParticipant>> participants;
    };

    struct RawEventGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<RawEvent> list;
        std::vector<std::string> notFound;
    };

    struct RawSetError
    {
        std::string type;
        std::optional<std::string> description;
        std::vector<std::string> properties;
    };

    struct RawCalendarSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::optional<std::string> onSuccessSetIsDefault;
    };

    struct RawCalendarSetResult
    {
        std::optional<bool> isDefault;
    };

    struct RawCalendarSetResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, RawCalendarSetResult> updated;
        std::unordered_map<std::string, RawSetError> notUpdated;
    };

    struct RawCalendarEventSetResult
    {
        std::optional<std::string> id;
    };

    struct RawSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, RawEventWrite> create;
        std::unordered_map<std::string, RawEventPatch> update;
        std::vector<std::string> destroy;
        bool sendSchedulingMessages = true;
    };

    struct RawSetResponse
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, RawCalendarEventSetResult> created;
        std::unordered_map<std::string, std::optional<RawCalendarEventSetResult>> updated;
        std::vector<std::string> destroyed;
        std::unordered_map<std::string, RawSetError> notCreated;
        std::unordered_map<std::string, RawSetError> notUpdated;
        std::unordered_map<std::string, RawSetError> notDestroyed;
    };
} // namespace javelin::jmap::api::detail

#define JAVELIN_GLZ_META(TYPE, ...)                                                                \
    template <> struct glz::meta<javelin::jmap::api::detail::TYPE>                                 \
    {                                                                                              \
        using T = javelin::jmap::api::detail::TYPE;                                                \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

JAVELIN_GLZ_META(RawCalendarRights, "mayReadFreeBusy", &T::mayReadFreeBusy, "mayReadItems",
                 &T::mayReadItems, "mayWriteAll", &T::mayWriteAll, "mayWriteOwn", &T::mayWriteOwn,
                 "mayUpdatePrivate", &T::mayUpdatePrivate, "mayRSVP", &T::mayRSVP, "mayShare",
                 &T::mayShare, "mayDelete", &T::mayDelete);
JAVELIN_GLZ_META(RawCalendar, "id", &T::id, "name", &T::name, "description", &T::description,
                 "color", &T::color, "sortOrder", &T::sortOrder, "isSubscribed", &T::isSubscribed,
                 "isVisible", &T::isVisible, "isDefault", &T::isDefault, "timeZone", &T::timeZone,
                 "defaultAlertsWithTime", &T::defaultAlertsWithTime, "defaultAlertsWithoutTime",
                 &T::defaultAlertsWithoutTime, "myRights", &T::myRights);
JAVELIN_GLZ_META(RawCalendarGetResponse, "accountId", &T::accountId, "state", &T::state, "list",
                 &T::list, "notFound", &T::notFound);
JAVELIN_GLZ_META(RawQueryFilter, "inCalendar", &T::inCalendar, "after", &T::after, "before",
                 &T::before, "text", &T::text, "uid", &T::uid);
JAVELIN_GLZ_META(RawQueryRequest, "accountId", &T::accountId, "filter", &T::filter,
                 "expandRecurrences", &T::expandRecurrences, "timeZone", &T::timeZone, "position",
                 &T::position, "limit", &T::limit, "calculateTotal", &T::calculateTotal);
JAVELIN_GLZ_META(RawQueryResponse, "accountId", &T::accountId, "queryState", &T::queryState,
                 "canCalculateChanges", &T::canCalculateChanges, "position", &T::position, "ids",
                 &T::ids, "total", &T::total, "limit", &T::limit);
JAVELIN_GLZ_META(RawResultReference, "resultOf", &T::resultOf, "name", &T::name, "path", &T::path);
JAVELIN_GLZ_META(RawEventGetRequest, "accountId", &T::accountId, "ids", &T::ids, "#ids",
                 &T::idsReference, "properties", &T::properties, "recurrenceOverridesBefore",
                 &T::recurrenceOverridesBefore, "recurrenceOverridesAfter",
                 &T::recurrenceOverridesAfter, "reduceParticipants", &T::reduceParticipants,
                 "timeZone", &T::timeZone);
JAVELIN_GLZ_META(RawRecurrenceDay, "@type", &T::type, "day", &T::day, "nthOfPeriod",
                 &T::nthOfPeriod);
JAVELIN_GLZ_META(RawRecurrenceRule, "@type", &T::type, "frequency", &T::frequency, "interval",
                 &T::interval, "rscale", &T::rscale, "skip", &T::skip, "firstDayOfWeek",
                 &T::firstDayOfWeek, "byDay", &T::byDay, "byMonthDay", &T::byMonthDay, "byMonth",
                 &T::byMonth, "byYearDay", &T::byYearDay, "byWeekNo", &T::byWeekNo, "byHour",
                 &T::byHour, "byMinute", &T::byMinute, "bySecond", &T::bySecond, "bySetPosition",
                 &T::bySetPosition, "count", &T::count, "until", &T::until);
JAVELIN_GLZ_META(RawOverride, "excluded", &T::excluded, "start", &T::start, "duration",
                 &T::duration, "title", &T::title);
JAVELIN_GLZ_META(RawParticipant, "@type", &T::type, "name", &T::name, "email", &T::email,
                 "calendarAddress", &T::calendarAddress, "participationStatus",
                 &T::participationStatus, "roles", &T::roles, "scheduleSequence",
                 &T::scheduleSequence, "scheduleUpdated", &T::scheduleUpdated);
JAVELIN_GLZ_META(RawLocation, "@type", &T::type, "name", &T::name);
JAVELIN_GLZ_META(RawTrigger, "@type", &T::type, "relativeTo", &T::relativeTo, "offset", &T::offset,
                 "when", &T::when);
JAVELIN_GLZ_META(RawAlert, "@type", &T::type, "action", &T::action, "trigger", &T::trigger,
                 "acknowledged", &T::acknowledged);
JAVELIN_GLZ_META(RawEvent, "@type", &T::type, "id", &T::id, "baseEventId", &T::baseEventId,
                 "recurrenceId", &T::recurrenceId, "uid", &T::uid, "calendarIds", &T::calendarIds,
                 "title", &T::title, "description", &T::description, "locations", &T::locations,
                 "start", &T::start, "duration", &T::duration, "timeZone", &T::timeZone,
                 "showWithoutTime", &T::showWithoutTime, "isDraft", &T::isDraft, "isOrigin",
                 &T::isOrigin, "utcStart", &T::utcStart, "utcEnd", &T::utcEnd, "recurrenceRule",
                 &T::recurrenceRule, "recurrenceOverrides", &T::recurrenceOverrides, "participants",
                 &T::participants, "useDefaultAlerts", &T::useDefaultAlerts, "alerts", &T::alerts);
JAVELIN_GLZ_META(RawEventWrite, "@type", &T::type, "uid", &T::uid, "calendarIds", &T::calendarIds,
                 "title", &T::title, "description", &T::description, "locations", &T::locations,
                 "start", &T::start, "duration", &T::duration, "timeZone", &T::timeZone,
                 "showWithoutTime", &T::showWithoutTime, "isDraft", &T::isDraft, "recurrenceRule",
                 &T::recurrenceRule, "recurrenceOverrides", &T::recurrenceOverrides, "participants",
                 &T::participants, "useDefaultAlerts", &T::useDefaultAlerts, "alerts", &T::alerts);
JAVELIN_GLZ_META(RawEventPatch, "calendarIds", &T::calendarIds, "title", &T::title, "description",
                 &T::description, "locations", &T::locations, "start", &T::start, "duration",
                 &T::duration, "timeZone", &T::timeZone, "showWithoutTime", &T::showWithoutTime,
                 "isDraft", &T::isDraft, "useDefaultAlerts", &T::useDefaultAlerts, "alerts",
                 &T::alerts, "recurrenceRule", &T::recurrenceRule, "recurrenceOverrides",
                 &T::recurrenceOverrides, "participants", &T::participants);
JAVELIN_GLZ_META(RawEventGetResponse, "accountId", &T::accountId, "state", &T::state, "list",
                 &T::list, "notFound", &T::notFound);
JAVELIN_GLZ_META(RawSetError, "type", &T::type, "description", &T::description, "properties",
                 &T::properties);
JAVELIN_GLZ_META(RawCalendarSetRequest, "accountId", &T::accountId, "ifInState", &T::ifInState,
                 "onSuccessSetIsDefault", &T::onSuccessSetIsDefault);
JAVELIN_GLZ_META(RawCalendarSetResult, "isDefault", &T::isDefault);
JAVELIN_GLZ_META(RawCalendarSetResponse, "accountId", &T::accountId, "oldState", &T::oldState,
                 "newState", &T::newState, "updated", &T::updated, "notUpdated", &T::notUpdated);
JAVELIN_GLZ_META(RawCalendarEventSetResult, "id", &T::id);
JAVELIN_GLZ_META(RawSetRequest, "accountId", &T::accountId, "ifInState", &T::ifInState, "create",
                 &T::create, "update", &T::update, "destroy", &T::destroy, "sendSchedulingMessages",
                 &T::sendSchedulingMessages);
JAVELIN_GLZ_META(RawSetResponse, "accountId", &T::accountId, "oldState", &T::oldState, "newState",
                 &T::newState, "created", &T::created, "updated", &T::updated, "destroyed",
                 &T::destroyed, "notCreated", &T::notCreated, "notUpdated", &T::notUpdated,
                 "notDestroyed", &T::notDestroyed);

#undef JAVELIN_GLZ_META

namespace javelin::jmap::api
{
    namespace
    {
        template <typename T> std::optional<std::string> serialize(const T& value)
        {
            std::string json;
            if (glz::write_json(value, json))
            {
                return std::nullopt;
            }
            return json;
        }

        template <typename T> ParsedEnvelope<T> parseRaw(std::string_view json)
        {
            std::string buffer{json};
            T value;
            if (const auto error =
                    glz::read<glz::opts{.error_on_unknown_keys = false}>(value, buffer))
            {
                return {.value = std::nullopt, .error = glz::format_error(error, buffer)};
            }
            return {.value = std::move(value), .error = std::nullopt};
        }

        calendar::CalendarRights rights(const detail::RawCalendarRights& value)
        {
            return {.mayReadFreeBusy = value.mayReadFreeBusy,
                    .mayReadItems = value.mayReadItems,
                    .mayWriteAll = value.mayWriteAll,
                    .mayWriteOwn = value.mayWriteOwn,
                    .mayUpdatePrivate = value.mayUpdatePrivate,
                    .mayRSVP = value.mayRSVP,
                    .mayShare = value.mayShare,
                    .mayDelete = value.mayDelete};
        }

        std::string frequency(const calendar::RecurrenceFrequency value)
        {
            switch (value)
            {
            case calendar::RecurrenceFrequency::Yearly:
                return "yearly";
            case calendar::RecurrenceFrequency::Monthly:
                return "monthly";
            case calendar::RecurrenceFrequency::Weekly:
                return "weekly";
            case calendar::RecurrenceFrequency::Daily:
                return "daily";
            case calendar::RecurrenceFrequency::Hourly:
                return "hourly";
            case calendar::RecurrenceFrequency::Minutely:
                return "minutely";
            case calendar::RecurrenceFrequency::Secondly:
                return "secondly";
            }
            return "daily";
        }

        calendar::RecurrenceFrequency frequency(std::string_view value)
        {
            if (value == "yearly")
                return calendar::RecurrenceFrequency::Yearly;
            if (value == "monthly")
                return calendar::RecurrenceFrequency::Monthly;
            if (value == "weekly")
                return calendar::RecurrenceFrequency::Weekly;
            if (value == "hourly")
                return calendar::RecurrenceFrequency::Hourly;
            if (value == "minutely")
                return calendar::RecurrenceFrequency::Minutely;
            if (value == "secondly")
                return calendar::RecurrenceFrequency::Secondly;
            return calendar::RecurrenceFrequency::Daily;
        }

        std::string skip(const calendar::RecurrenceSkip value)
        {
            switch (value)
            {
            case calendar::RecurrenceSkip::Omit:
                return "omit";
            case calendar::RecurrenceSkip::Backward:
                return "backward";
            case calendar::RecurrenceSkip::Forward:
                return "forward";
            }
            return "omit";
        }

        calendar::RecurrenceSkip recurrenceSkip(const std::string_view value)
        {
            if (value == "backward")
                return calendar::RecurrenceSkip::Backward;
            if (value == "forward")
                return calendar::RecurrenceSkip::Forward;
            return calendar::RecurrenceSkip::Omit;
        }

        std::string weekday(const calendar::Weekday value)
        {
            switch (value)
            {
            case calendar::Weekday::Monday:
                return "mo";
            case calendar::Weekday::Tuesday:
                return "tu";
            case calendar::Weekday::Wednesday:
                return "we";
            case calendar::Weekday::Thursday:
                return "th";
            case calendar::Weekday::Friday:
                return "fr";
            case calendar::Weekday::Saturday:
                return "sa";
            case calendar::Weekday::Sunday:
                return "su";
            }
            return "mo";
        }

        calendar::Weekday weekday(const std::string_view value)
        {
            if (value == "tu")
                return calendar::Weekday::Tuesday;
            if (value == "we")
                return calendar::Weekday::Wednesday;
            if (value == "th")
                return calendar::Weekday::Thursday;
            if (value == "fr")
                return calendar::Weekday::Friday;
            if (value == "sa")
                return calendar::Weekday::Saturday;
            if (value == "su")
                return calendar::Weekday::Sunday;
            return calendar::Weekday::Monday;
        }

        template <typename T>
        std::optional<std::vector<T>> optionalValues(const std::vector<T>& values)
        {
            return values.empty() ? std::nullopt : std::optional{values};
        }

        bool isImportedAllDayEvent(const detail::RawEvent& value)
        {
            if (value.showWithoutTime)
                return true;
            if (value.start.size() < 19 || value.start.compare(11, 8, "00:00:00") != 0)
                return false;
            if (value.duration.size() < 3 || value.duration.front() != 'P')
                return false;
            const auto unit = value.duration.back();
            if (unit != 'D' && unit != 'W')
                return false;
            const std::string_view amount{value.duration.data() + 1, value.duration.size() - 2};
            return !amount.empty() &&
                   std::ranges::all_of(amount, [](const char character)
                                       { return character >= '0' && character <= '9'; }) &&
                   std::ranges::any_of(amount,
                                       [](const char character) { return character != '0'; });
        }

        detail::RawEvent rawEvent(const calendar::CalendarEvent& value)
        {
            detail::RawEvent raw{
                .type = "Event",
                .id = value.id,
                .baseEventId = value.baseEventId,
                .recurrenceId =
                    value.recurrenceId ? std::optional{value.recurrenceId->value} : std::nullopt,
                .uid = value.uid,
                .calendarIds = value.calendarIds,
                .title = value.title,
                .description = value.description,
                .locations = {},
                .start = value.start.value,
                .duration = value.duration.value,
                .timeZone = value.timeZone ? std::optional{value.timeZone->value} : std::nullopt,
                .showWithoutTime = value.showWithoutTime,
                .isDraft = value.isDraft,
                .isOrigin = value.isOrigin,
                .useDefaultAlerts = value.useDefaultAlerts,
                .alerts = {},
                .utcStart = value.utcStart ? std::optional{value.utcStart->value} : std::nullopt,
                .utcEnd = value.utcEnd ? std::optional{value.utcEnd->value} : std::nullopt,
                .recurrenceRule = std::nullopt,
                .recurrenceOverrides = {},
                .participants = {}};
            for (const auto& [id, alert] : value.alerts)
            {
                raw.alerts.emplace(
                    id, detail::RawAlert{
                            .type = "Alert",
                            .action = alert.action,
                            .trigger = {.type = alert.triggerKind ==
                                                        calendar::AlertTriggerKind::Absolute
                                                    ? "AbsoluteTrigger"
                                                    : "OffsetTrigger",
                                        .relativeTo = alert.relativeTo,
                                        .offset = alert.offset ? std::optional{alert.offset->value}
                                                               : std::nullopt,
                                        .when = alert.when ? std::optional{alert.when->value}
                                                           : std::nullopt},
                            .acknowledged = alert.acknowledged
                                                ? std::optional{alert.acknowledged->value}
                                                : std::nullopt});
            }
            if (value.location)
            {
                raw.locations.emplace(
                    "location", detail::RawLocation{.type = "Location", .name = *value.location});
            }
            if (value.recurrenceRule)
            {
                std::vector<detail::RawRecurrenceDay> byDay;
                byDay.reserve(value.recurrenceRule->byDay.size());
                for (const auto& day : value.recurrenceRule->byDay)
                    byDay.push_back(
                        {.type = "NDay", .day = weekday(day.day), .nthOfPeriod = day.nthOfPeriod});
                raw.recurrenceRule = detail::RawRecurrenceRule{
                    .type = "RecurrenceRule",
                    .frequency = frequency(value.recurrenceRule->frequency),
                    .interval = value.recurrenceRule->interval,
                    .rscale = value.recurrenceRule->rscale,
                    .skip = value.recurrenceRule->skip
                                ? std::optional{skip(*value.recurrenceRule->skip)}
                                : std::nullopt,
                    .firstDayOfWeek =
                        value.recurrenceRule->firstDayOfWeek
                            ? std::optional{weekday(*value.recurrenceRule->firstDayOfWeek)}
                            : std::nullopt,
                    .byDay = optionalValues(byDay),
                    .byMonthDay = optionalValues(value.recurrenceRule->byMonthDay),
                    .byMonth = optionalValues(value.recurrenceRule->byMonth),
                    .byYearDay = optionalValues(value.recurrenceRule->byYearDay),
                    .byWeekNo = optionalValues(value.recurrenceRule->byWeekNo),
                    .byHour = optionalValues(value.recurrenceRule->byHour),
                    .byMinute = optionalValues(value.recurrenceRule->byMinute),
                    .bySecond = optionalValues(value.recurrenceRule->bySecond),
                    .bySetPosition = optionalValues(value.recurrenceRule->bySetPosition),
                    .count = value.recurrenceRule->count,
                    .until = value.recurrenceRule->until
                                 ? std::optional{value.recurrenceRule->until->value}
                                 : std::nullopt};
            }
            for (const auto& [id, valueOverride] : value.recurrenceOverrides)
            {
                raw.recurrenceOverrides.emplace(
                    id, detail::RawOverride{
                            .excluded = valueOverride.excluded,
                            .start = valueOverride.start ? std::optional{valueOverride.start->value}
                                                         : std::nullopt,
                            .duration = valueOverride.duration
                                            ? std::optional{valueOverride.duration->value}
                                            : std::nullopt,
                            .title = valueOverride.title});
            }
            for (const auto& attendee : value.attendees)
            {
                raw.participants.emplace(
                    attendee.id,
                    detail::RawParticipant{
                        .type = "Participant",
                        .name = attendee.name,
                        .email = attendee.email,
                        .calendarAddress = attendee.calendarAddress,
                        .participationStatus = attendee.participationStatus,
                        .roles = {{"owner", attendee.isOwner}, {"attendee", attendee.isAttendee}},
                        .scheduleSequence = attendee.scheduleSequence,
                        .scheduleUpdated = attendee.scheduleUpdated
                                               ? std::optional{attendee.scheduleUpdated->value}
                                               : std::nullopt});
            }
            return raw;
        }

        detail::RawEventWrite rawEventWrite(const calendar::CalendarEvent& value)
        {
            const auto raw = rawEvent(value);
            return {.type = raw.type,
                    .uid = raw.uid.empty() ? std::nullopt : std::optional{raw.uid},
                    .calendarIds = raw.calendarIds,
                    .title = raw.title,
                    .description = raw.description,
                    .locations = raw.locations,
                    .start = raw.start,
                    .duration = raw.duration,
                    .timeZone = raw.timeZone,
                    .showWithoutTime = raw.showWithoutTime,
                    .isDraft = raw.isDraft,
                    .useDefaultAlerts = raw.useDefaultAlerts,
                    .alerts = raw.alerts,
                    .recurrenceRule = raw.recurrenceRule,
                    .recurrenceOverrides = raw.recurrenceOverrides,
                    .participants = raw.participants};
        }

        template <typename T> std::optional<T> changed(const T& previous, const T& current)
        {
            return previous == current ? std::nullopt : std::optional{current};
        }

        template <typename T>
        std::optional<std::variant<std::nullptr_t, T>>
        changedNullable(const std::optional<T>& previous, const std::optional<T>& current)
        {
            if (previous == current)
                return std::nullopt;
            return current ? std::variant<std::nullptr_t, T>{*current}
                           : std::variant<std::nullptr_t, T>{nullptr};
        }

        detail::RawEventPatch rawEventPatch(const calendar::CalendarEvent& previous,
                                            const calendar::CalendarEvent& current)
        {
            const auto before = rawEventWrite(previous);
            const auto after = rawEventWrite(current);
            return {.calendarIds = changed(before.calendarIds, after.calendarIds),
                    .title = changed(before.title, after.title),
                    .description = changedNullable(before.description, after.description),
                    .locations = changed(before.locations, after.locations),
                    .start = changed(before.start, after.start),
                    .duration = changed(before.duration, after.duration),
                    .timeZone = changedNullable(before.timeZone, after.timeZone),
                    .showWithoutTime = changed(before.showWithoutTime, after.showWithoutTime),
                    .isDraft = changed(before.isDraft, after.isDraft),
                    .useDefaultAlerts = changed(before.useDefaultAlerts, after.useDefaultAlerts),
                    .alerts = changed(before.alerts, after.alerts),
                    .recurrenceRule = changedNullable(before.recurrenceRule, after.recurrenceRule),
                    .recurrenceOverrides =
                        changed(before.recurrenceOverrides, after.recurrenceOverrides),
                    .participants = changed(before.participants, after.participants)};
        }

        calendar::CalendarEvent event(const std::string& accountId, const detail::RawEvent& raw)
        {
            calendar::CalendarEvent value{
                .accountId = accountId,
                .id = raw.id,
                .baseEventId = raw.baseEventId,
                .recurrenceId =
                    raw.recurrenceId
                        ? std::optional<calendar::LocalDateTime>{{.value = *raw.recurrenceId}}
                        : std::nullopt,
                .uid = raw.uid,
                .calendarIds = raw.calendarIds,
                .title = raw.title,
                .description = raw.description,
                .location = raw.locations.empty()
                                ? std::nullopt
                                : std::optional{raw.locations.begin()->second.name},
                .start = {.value = raw.start},
                .duration = {.value = raw.duration},
                .timeZone = raw.timeZone
                                ? std::optional<calendar::TimeZoneId>{{.value = *raw.timeZone}}
                                : std::nullopt,
                .showWithoutTime = isImportedAllDayEvent(raw),
                .isDraft = raw.isDraft,
                .isOrigin = raw.isOrigin,
                .useDefaultAlerts = raw.useDefaultAlerts,
                .alerts = {},
                .utcStart = raw.utcStart
                                ? std::optional<calendar::UtcInstant>{{.value = *raw.utcStart}}
                                : std::nullopt,
                .utcEnd = raw.utcEnd ? std::optional<calendar::UtcInstant>{{.value = *raw.utcEnd}}
                                     : std::nullopt,
                .recurrenceRule = std::nullopt,
                .recurrenceOverrides = {},
                .attendees = {}};
            for (const auto& [id, alert] : raw.alerts)
            {
                value.alerts.emplace(
                    id,
                    calendar::Alert{
                        .id = id,
                        .action = alert.action,
                        .triggerKind = alert.trigger.type == "AbsoluteTrigger"
                                           ? calendar::AlertTriggerKind::Absolute
                                           : calendar::AlertTriggerKind::Offset,
                        .relativeTo = alert.trigger.relativeTo,
                        .offset =
                            alert.trigger.offset
                                ? std::optional<calendar::Duration>{{.value =
                                                                         *alert.trigger.offset}}
                                : std::nullopt,
                        .when = alert.trigger.when
                                    ? std::optional<calendar::UtcInstant>{{.value =
                                                                               *alert.trigger.when}}
                                    : std::nullopt,
                        .acknowledged =
                            alert.acknowledged
                                ? std::optional<calendar::UtcInstant>{{.value =
                                                                           *alert.acknowledged}}
                                : std::nullopt});
            }
            if (raw.recurrenceRule)
            {
                std::vector<calendar::RecurrenceDay> byDay;
                if (raw.recurrenceRule->byDay)
                {
                    byDay.reserve(raw.recurrenceRule->byDay->size());
                    for (const auto& day : *raw.recurrenceRule->byDay)
                        byDay.push_back({.day = weekday(day.day), .nthOfPeriod = day.nthOfPeriod});
                }
                value.recurrenceRule = calendar::RecurrenceRule{
                    .frequency = frequency(raw.recurrenceRule->frequency),
                    .interval = raw.recurrenceRule->interval,
                    .rscale = raw.recurrenceRule->rscale,
                    .skip = raw.recurrenceRule->skip
                                ? std::optional{recurrenceSkip(*raw.recurrenceRule->skip)}
                                : std::nullopt,
                    .firstDayOfWeek =
                        raw.recurrenceRule->firstDayOfWeek
                            ? std::optional{weekday(*raw.recurrenceRule->firstDayOfWeek)}
                            : std::nullopt,
                    .byDay = std::move(byDay),
                    .byMonthDay =
                        raw.recurrenceRule->byMonthDay.value_or(std::vector<std::int32_t>{}),
                    .byMonth = raw.recurrenceRule->byMonth.value_or(std::vector<std::string>{}),
                    .byYearDay =
                        raw.recurrenceRule->byYearDay.value_or(std::vector<std::int32_t>{}),
                    .byWeekNo = raw.recurrenceRule->byWeekNo.value_or(std::vector<std::int32_t>{}),
                    .byHour = raw.recurrenceRule->byHour.value_or(std::vector<std::uint32_t>{}),
                    .byMinute = raw.recurrenceRule->byMinute.value_or(std::vector<std::uint32_t>{}),
                    .bySecond = raw.recurrenceRule->bySecond.value_or(std::vector<std::uint32_t>{}),
                    .bySetPosition =
                        raw.recurrenceRule->bySetPosition.value_or(std::vector<std::int32_t>{}),
                    .count = raw.recurrenceRule->count,
                    .until =
                        raw.recurrenceRule->until
                            ? std::optional<calendar::LocalDateTime>{{.value = *raw.recurrenceRule
                                                                                    ->until}}
                            : std::nullopt};
            }
            for (const auto& [id, rawOverride] : raw.recurrenceOverrides)
            {
                value.recurrenceOverrides.emplace(
                    id,
                    calendar::RecurrenceOverride{
                        .excluded = rawOverride.excluded,
                        .start =
                            rawOverride.start
                                ? std::optional<calendar::LocalDateTime>{{.value =
                                                                              *rawOverride.start}}
                                : std::nullopt,
                        .duration =
                            rawOverride.duration
                                ? std::optional<calendar::Duration>{{.value =
                                                                         *rawOverride.duration}}
                                : std::nullopt,
                        .title = rawOverride.title});
            }
            for (const auto& [id, participant] : raw.participants)
            {
                value.attendees.push_back(calendar::Attendee{
                    .id = id,
                    .name = participant.name,
                    .email = participant.email,
                    .calendarAddress = participant.calendarAddress,
                    .participationStatus = participant.participationStatus,
                    .isOwner = participant.roles.contains("owner") && participant.roles.at("owner"),
                    .isAttendee =
                        participant.roles.contains("attendee") && participant.roles.at("attendee"),
                    .scheduleSequence = participant.scheduleSequence,
                    .scheduleUpdated =
                        participant.scheduleUpdated
                            ? std::optional<calendar::UtcInstant>{{.value = *participant
                                                                                 .scheduleUpdated}}
                            : std::nullopt});
            }
            return value;
        }

        CalendarSetError setError(const detail::RawSetError& raw)
        {
            CalendarSetErrorType type = CalendarSetErrorType::Unknown;
            if (raw.type == "invalidArguments")
                type = CalendarSetErrorType::InvalidArguments;
            else if (raw.type == "invalidProperties")
                type = CalendarSetErrorType::InvalidProperties;
            else if (raw.type == "forbidden")
                type = CalendarSetErrorType::Forbidden;
            else if (raw.type == "notFound")
                type = CalendarSetErrorType::NotFound;
            else if (raw.type == "stateMismatch")
                type = CalendarSetErrorType::StateMismatch;
            else if (raw.type == "noSupportedScheduleMethods")
                type = CalendarSetErrorType::NoSupportedScheduleMethods;
            return {.type = type, .description = raw.description, .properties = raw.properties};
        }
    } // namespace

    std::optional<MethodRequest<CalendarGetResponse>> calendarGet(const GetRequest& request)
    {
        const auto arguments = serializeGetRequest(request);
        return arguments ? std::optional{MethodRequest<CalendarGetResponse>{
                               .name = "Calendar/get", .arguments = *arguments}}
                         : std::nullopt;
    }

    std::optional<MethodRequest<CalendarChangesResponse>>
    calendarChanges(const ChangesRequest& request)
    {
        const auto arguments = serializeChangesRequest(request);
        return arguments ? std::optional{MethodRequest<CalendarChangesResponse>{
                               .name = "Calendar/changes", .arguments = *arguments}}
                         : std::nullopt;
    }

    std::optional<MethodRequest<CalendarSetResponse>> calendarSet(const CalendarSetRequest& request)
    {
        const auto arguments = serialize(
            detail::RawCalendarSetRequest{.accountId = request.accountId,
                                          .ifInState = request.ifInState,
                                          .onSuccessSetIsDefault = request.onSuccessSetIsDefault});
        return arguments ? std::optional{MethodRequest<CalendarSetResponse>{
                               .name = "Calendar/set", .arguments = *arguments}}
                         : std::nullopt;
    }

    std::optional<MethodRequest<CalendarEventQueryResponse>>
    calendarEventQuery(const CalendarEventQueryRequest& request)
    {
        const auto arguments = serialize(detail::RawQueryRequest{
            .accountId = request.accountId,
            .filter = {.inCalendar = request.filter.inCalendar,
                       .after = request.filter.after ? std::optional{request.filter.after->value}
                                                     : std::nullopt,
                       .before = request.filter.before ? std::optional{request.filter.before->value}
                                                       : std::nullopt,
                       .text = request.filter.text,
                       .uid = request.filter.uid},
            .expandRecurrences = request.expandRecurrences,
            .timeZone = request.timeZone.value,
            .position = request.position,
            .limit = request.limit,
            .calculateTotal = request.calculateTotal});
        return arguments ? std::optional{MethodRequest<CalendarEventQueryResponse>{
                               .name = "CalendarEvent/query", .arguments = *arguments}}
                         : std::nullopt;
    }

    std::optional<MethodRequest<CalendarEventGetResponse>>
    calendarEventGet(const CalendarEventGetRequest& request)
    {
        const auto arguments = serialize(detail::RawEventGetRequest{
            .accountId = request.accountId,
            .ids = request.ids,
            .idsReference = request.idsReference ? std::optional{detail::RawResultReference{
                                                       .resultOf = request.idsReference->resultOf,
                                                       .name = request.idsReference->name,
                                                       .path = request.idsReference->path,
                                                   }}
                                                 : std::nullopt,
            .properties = request.properties,
            .recurrenceOverridesBefore =
                request.recurrenceOverridesBefore
                    ? std::optional{request.recurrenceOverridesBefore->value}
                    : std::nullopt,
            .recurrenceOverridesAfter = request.recurrenceOverridesAfter
                                            ? std::optional{request.recurrenceOverridesAfter->value}
                                            : std::nullopt,
            .reduceParticipants = request.reduceParticipants,
            .timeZone = request.timeZone.value});
        return arguments ? std::optional{MethodRequest<CalendarEventGetResponse>{
                               .name = "CalendarEvent/get", .arguments = *arguments}}
                         : std::nullopt;
    }

    std::optional<MethodRequest<CalendarEventChangesResponse>>
    calendarEventChanges(const ChangesRequest& request)
    {
        const auto arguments = serializeChangesRequest(request);
        return arguments ? std::optional{MethodRequest<CalendarEventChangesResponse>{
                               .name = "CalendarEvent/changes", .arguments = *arguments}}
                         : std::nullopt;
    }

    std::optional<MethodRequest<CalendarEventSetResponse>>
    calendarEventSet(const CalendarEventSetRequest& request)
    {
        detail::RawSetRequest raw{.accountId = request.accountId,
                                  .ifInState = request.ifInState,
                                  .create = {},
                                  .update = {},
                                  .destroy = request.destroy,
                                  .sendSchedulingMessages = request.sendSchedulingMessages};
        for (const auto& [id, value] : request.create)
            raw.create.emplace(id, rawEventWrite(value));
        for (const auto& [id, value] : request.update)
            raw.update.emplace(id, rawEventPatch(value.previous, value.event));
        const auto arguments = serialize(raw);
        return arguments ? std::optional{MethodRequest<CalendarEventSetResponse>{
                               .name = "CalendarEvent/set", .arguments = *arguments}}
                         : std::nullopt;
    }

    ParsedEnvelope<CalendarGetResponse> parseCalendarGetResponse(std::string_view json)
    {
        auto raw = parseRaw<detail::RawCalendarGetResponse>(json);
        if (!raw.ok())
            return {.value = std::nullopt, .error = raw.error};
        CalendarGetResponse result{.accountId = raw.value->accountId,
                                   .state = raw.value->state,
                                   .list = {},
                                   .notFound = raw.value->notFound};
        for (const auto& item : raw.value->list)
        {
            calendar::Calendar parsed{
                .accountId = result.accountId,
                .id = item.id,
                .name = item.name,
                .description = item.description,
                .color = item.color,
                .sortOrder = item.sortOrder,
                .isSubscribed = item.isSubscribed,
                .isVisible = item.isVisible,
                .isDefault = item.isDefault,
                .timeZone = item.timeZone
                                ? std::optional<calendar::TimeZoneId>{{.value = *item.timeZone}}
                                : std::nullopt,
                .defaultAlertsWithTime = {},
                .defaultAlertsWithoutTime = {},
                .myRights = rights(item.myRights)};
            const auto appendAlerts = [](const auto& source, auto& destination)
            {
                for (const auto& [id, alert] : source)
                    destination.emplace(
                        id,
                        calendar::Alert{
                            .id = id,
                            .action = alert.action,
                            .triggerKind = alert.trigger.type == "AbsoluteTrigger"
                                               ? calendar::AlertTriggerKind::Absolute
                                               : calendar::AlertTriggerKind::Offset,
                            .relativeTo = alert.trigger.relativeTo,
                            .offset =
                                alert.trigger.offset
                                    ? std::optional<calendar::Duration>{{.value =
                                                                             *alert.trigger.offset}}
                                    : std::nullopt,
                            .when =
                                alert.trigger.when
                                    ? std::optional<calendar::UtcInstant>{{.value =
                                                                               *alert.trigger.when}}
                                    : std::nullopt,
                            .acknowledged =
                                alert.acknowledged
                                    ? std::optional<calendar::UtcInstant>{{.value =
                                                                               *alert.acknowledged}}
                                    : std::nullopt});
            };
            appendAlerts(item.defaultAlertsWithTime, parsed.defaultAlertsWithTime);
            appendAlerts(item.defaultAlertsWithoutTime, parsed.defaultAlertsWithoutTime);
            result.list.push_back(std::move(parsed));
        }
        return {.value = std::move(result), .error = std::nullopt};
    }

    ParsedEnvelope<CalendarChangesResponse> parseCalendarChangesResponse(std::string_view json)
    {
        const auto parsed = parseChangesResponse(json);
        if (!parsed.ok())
            return {.value = std::nullopt, .error = parsed.error};
        CalendarChangesResponse result;
        static_cast<ChangesResponse&>(result) = *parsed.value;
        return {.value = std::move(result), .error = std::nullopt};
    }

    ParsedEnvelope<CalendarSetResponse> parseCalendarSetResponse(std::string_view json)
    {
        auto raw = parseRaw<detail::RawCalendarSetResponse>(json);
        if (!raw.ok())
            return {.value = std::nullopt, .error = raw.error};
        CalendarSetResponse result{.accountId = std::move(raw.value->accountId),
                                   .oldState = std::move(raw.value->oldState),
                                   .newState = std::move(raw.value->newState),
                                   .updated = {},
                                   .notUpdated = {}};
        for (auto& [id, value] : raw.value->updated)
            result.updated.emplace(std::move(id),
                                   CalendarSetResponse::SetResult{.isDefault = value.isDefault});
        for (auto& [id, value] : raw.value->notUpdated)
            result.notUpdated.emplace(std::move(id), setError(value));
        return {.value = std::move(result), .error = std::nullopt};
    }

    ParsedEnvelope<CalendarEventQueryResponse>
    parseCalendarEventQueryResponse(std::string_view json)
    {
        auto raw = parseRaw<detail::RawQueryResponse>(json);
        if (!raw.ok())
            return {.value = std::nullopt, .error = raw.error};
        return {.value = CalendarEventQueryResponse{.accountId = std::move(raw.value->accountId),
                                                    .queryState = std::move(raw.value->queryState),
                                                    .canCalculateChanges =
                                                        raw.value->canCalculateChanges,
                                                    .position = raw.value->position,
                                                    .ids = std::move(raw.value->ids),
                                                    .total = raw.value->total,
                                                    .limit = raw.value->limit},
                .error = std::nullopt};
    }

    ParsedEnvelope<CalendarEventGetResponse> parseCalendarEventGetResponse(std::string_view json)
    {
        auto raw = parseRaw<detail::RawEventGetResponse>(json);
        if (!raw.ok())
            return {.value = std::nullopt, .error = raw.error};
        CalendarEventGetResponse result{.accountId = raw.value->accountId,
                                        .state = raw.value->state,
                                        .list = {},
                                        .notFound = raw.value->notFound};
        for (const auto& item : raw.value->list)
            result.list.push_back(event(result.accountId, item));
        return {.value = std::move(result), .error = std::nullopt};
    }

    ParsedEnvelope<CalendarEventChangesResponse>
    parseCalendarEventChangesResponse(std::string_view json)
    {
        const auto parsed = parseChangesResponse(json);
        if (!parsed.ok())
            return {.value = std::nullopt, .error = parsed.error};
        CalendarEventChangesResponse result;
        static_cast<ChangesResponse&>(result) = *parsed.value;
        return {.value = std::move(result), .error = std::nullopt};
    }

    ParsedEnvelope<CalendarEventSetResponse> parseCalendarEventSetResponse(std::string_view json)
    {
        auto raw = parseRaw<detail::RawSetResponse>(json);
        if (!raw.ok())
            return {.value = std::nullopt, .error = raw.error};
        CalendarEventSetResponse result{.accountId = raw.value->accountId,
                                        .oldState = raw.value->oldState,
                                        .newState = raw.value->newState,
                                        .created = {},
                                        .updated = {},
                                        .destroyed = raw.value->destroyed,
                                        .notCreated = {},
                                        .notUpdated = {},
                                        .notDestroyed = {}};
        for (const auto& [id, value] : raw.value->created)
            result.created.emplace(id, CalendarEventSetResponse::SetResult{.id = value.id});
        for (const auto& [id, value] : raw.value->updated)
            result.updated.emplace(
                id, value ? std::optional{CalendarEventSetResponse::SetResult{.id = value->id}}
                          : std::nullopt);
        for (const auto& [id, value] : raw.value->notCreated)
            result.notCreated.emplace(id, setError(value));
        for (const auto& [id, value] : raw.value->notUpdated)
            result.notUpdated.emplace(id, setError(value));
        for (const auto& [id, value] : raw.value->notDestroyed)
            result.notDestroyed.emplace(id, setError(value));
        return {.value = std::move(result), .error = std::nullopt};
    }

    std::optional<std::string> serializeCalendarEventDocument(const calendar::CalendarEvent& value)
    {
        return serialize(rawEvent(value));
    }

    bool calendarEventWritablePropertiesEqual(const calendar::CalendarEvent& left,
                                              const calendar::CalendarEvent& right)
    {
        return rawEventWrite(left) == rawEventWrite(right);
    }

    ParsedEnvelope<calendar::CalendarEvent>
    parseCalendarEventDocument(const std::string_view accountId, const std::string_view json)
    {
        auto raw = parseRaw<detail::RawEvent>(json);
        if (!raw.ok())
        {
            return {.value = std::nullopt, .error = raw.error};
        }
        return {.value = event(std::string{accountId}, *raw.value), .error = std::nullopt};
    }
} // namespace javelin::jmap::api
