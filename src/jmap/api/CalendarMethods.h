#pragma once

#include "jmap/api/MailMethods.h"
#include "jmap/calendar/CalendarTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::api
{
    struct ParticipantIdentityGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<calendar::ParticipantIdentity> list;
        std::vector<std::string> notFound;
    };

    struct CalendarGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<calendar::Calendar> list;
        std::vector<std::string> notFound;
    };

    struct CalendarChangesResponse : ChangesResponse
    {
    };

    struct CalendarEventQueryFilter
    {
        std::optional<std::string> inCalendar = std::nullopt;
        std::optional<calendar::LocalDateTime> after = std::nullopt;
        std::optional<calendar::LocalDateTime> before = std::nullopt;
        std::optional<std::string> text = std::nullopt;
        std::optional<std::string> uid = std::nullopt;
    };

    struct CalendarEventQueryRequest
    {
        std::string accountId;
        CalendarEventQueryFilter filter;
        bool expandRecurrences = true;
        calendar::TimeZoneId timeZone;
        std::uint64_t position = 0;
        std::optional<std::uint64_t> limit;
        bool calculateTotal = true;
    };

    struct CalendarEventQueryResponse
    {
        std::string accountId;
        std::string queryState;
        bool canCalculateChanges = false;
        std::uint64_t position = 0;
        std::vector<std::string> ids;
        std::optional<std::uint64_t> total;
        std::optional<std::uint64_t> limit;
    };

    struct CalendarEventGetRequest
    {
        std::string accountId;
        std::optional<std::vector<std::string>> ids;
        std::optional<GetRequest::ResultReference> idsReference;
        std::optional<std::vector<std::string>> properties;
        std::optional<calendar::UtcInstant> recurrenceOverridesBefore;
        std::optional<calendar::UtcInstant> recurrenceOverridesAfter;
        bool reduceParticipants = false;
        calendar::TimeZoneId timeZone{.value = "Etc/UTC"};
    };

    struct CalendarEventGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<calendar::CalendarEvent> list;
        std::vector<std::string> notFound;
    };

    struct CalendarEventChangesResponse : ChangesResponse
    {
    };

    struct CalendarEventNotificationQueryFilter
    {
        std::optional<calendar::UtcInstant> after;
        std::optional<calendar::UtcInstant> before;
        std::optional<calendar::CalendarEventNotificationType> type;
        std::optional<std::vector<std::string>> calendarEventIds;
    };

    struct CalendarEventNotificationQueryRequest
    {
        std::string accountId;
        CalendarEventNotificationQueryFilter filter;
        std::uint64_t position = 0;
        std::optional<std::uint64_t> limit;
        bool calculateTotal = true;
    };

    struct CalendarEventNotificationQueryResponse
    {
        std::string accountId;
        std::string queryState;
        bool canCalculateChanges = false;
        std::uint64_t position = 0;
        std::vector<std::string> ids;
        std::optional<std::uint64_t> total;
        std::optional<std::uint64_t> limit;
    };

    struct CalendarEventNotificationGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<calendar::CalendarEventNotification> list;
        std::vector<std::string> notFound;
    };

    struct CalendarEventNotificationChangesResponse : ChangesResponse
    {
    };

    enum class CalendarSetErrorType
    {
        InvalidArguments,
        InvalidProperties,
        Forbidden,
        NotFound,
        StateMismatch,
        NoSupportedScheduleMethods,
        Unknown,
    };

    struct CalendarSetError
    {
        CalendarSetErrorType type = CalendarSetErrorType::Unknown;
        std::optional<std::string> description;
        std::vector<std::string> properties;
    };

    struct CalendarSetRequest
    {
        struct Update
        {
            std::optional<bool> isSubscribed;
        };

        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, calendar::Calendar> create;
        std::unordered_map<std::string, Update> update;
        std::vector<std::string> destroy;
        bool onDestroyRemoveEvents = false;
        std::optional<std::string> onSuccessSetIsDefault;
    };

    struct CalendarSetResponse
    {
        struct SetResult
        {
            std::optional<std::string> id;
            std::optional<bool> isDefault;
        };

        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, SetResult> created;
        std::unordered_map<std::string, SetResult> updated;
        std::vector<std::string> destroyed;
        std::unordered_map<std::string, CalendarSetError> notCreated;
        std::unordered_map<std::string, CalendarSetError> notUpdated;
        std::unordered_map<std::string, CalendarSetError> notDestroyed;
    };

    struct CalendarEventSetRequest
    {
        struct Update
        {
            calendar::CalendarEvent previous;
            calendar::CalendarEvent event;
        };

        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, calendar::CalendarEvent> create;
        std::unordered_map<std::string, Update> update;
        std::vector<std::string> destroy;
        bool sendSchedulingMessages = true;
    };

    struct CalendarEventSetResponse
    {
        struct SetResult
        {
            std::optional<std::string> id;
        };

        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, SetResult> created;
        std::unordered_map<std::string, std::optional<SetResult>> updated;
        std::vector<std::string> destroyed;
        std::unordered_map<std::string, CalendarSetError> notCreated;
        std::unordered_map<std::string, CalendarSetError> notUpdated;
        std::unordered_map<std::string, CalendarSetError> notDestroyed;
    };

    [[nodiscard]] std::optional<MethodRequest<ParticipantIdentityGetResponse>>
    participantIdentityGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarGetResponse>>
    calendarGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarChangesResponse>>
    calendarChanges(const ChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarSetResponse>>
    calendarSet(const CalendarSetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventQueryResponse>>
    calendarEventQuery(const CalendarEventQueryRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventGetResponse>>
    calendarEventGet(const CalendarEventGetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventChangesResponse>>
    calendarEventChanges(const ChangesRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventSetResponse>>
    calendarEventSet(const CalendarEventSetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventNotificationQueryResponse>>
    calendarEventNotificationQuery(const CalendarEventNotificationQueryRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventNotificationGetResponse>>
    calendarEventNotificationGet(const GetRequest& request);
    [[nodiscard]] std::optional<MethodRequest<CalendarEventNotificationChangesResponse>>
    calendarEventNotificationChanges(const ChangesRequest& request);

    [[nodiscard]] ParsedEnvelope<ParticipantIdentityGetResponse>
    parseParticipantIdentityGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarGetResponse>
    parseCalendarGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarChangesResponse>
    parseCalendarChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarSetResponse>
    parseCalendarSetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventQueryResponse>
    parseCalendarEventQueryResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventGetResponse>
    parseCalendarEventGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventChangesResponse>
    parseCalendarEventChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventSetResponse>
    parseCalendarEventSetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventNotificationQueryResponse>
    parseCalendarEventNotificationQueryResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventNotificationGetResponse>
    parseCalendarEventNotificationGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<CalendarEventNotificationChangesResponse>
    parseCalendarEventNotificationChangesResponse(std::string_view json);
    [[nodiscard]] std::optional<std::string>
    serializeCalendarEventDocument(const calendar::CalendarEvent& event);
    [[nodiscard]] bool calendarEventWritablePropertiesEqual(const calendar::CalendarEvent& left,
                                                            const calendar::CalendarEvent& right);
    [[nodiscard]] ParsedEnvelope<calendar::CalendarEvent>
    parseCalendarEventDocument(std::string_view accountId, std::string_view json);

    template <> struct MethodResponseTraits<ParticipantIdentityGetResponse>
    {
        static constexpr std::string_view methodName = "ParticipantIdentity/get";
        static ParsedEnvelope<ParticipantIdentityGetResponse> parse(std::string_view json)
        {
            return parseParticipantIdentityGetResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarGetResponse>
    {
        static constexpr std::string_view methodName = "Calendar/get";
        static ParsedEnvelope<CalendarGetResponse> parse(std::string_view json)
        {
            return parseCalendarGetResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarChangesResponse>
    {
        static constexpr std::string_view methodName = "Calendar/changes";
        static ParsedEnvelope<CalendarChangesResponse> parse(std::string_view json)
        {
            return parseCalendarChangesResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarSetResponse>
    {
        static constexpr std::string_view methodName = "Calendar/set";
        static ParsedEnvelope<CalendarSetResponse> parse(std::string_view json)
        {
            return parseCalendarSetResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventQueryResponse>
    {
        static constexpr std::string_view methodName = "CalendarEvent/query";
        static ParsedEnvelope<CalendarEventQueryResponse> parse(std::string_view json)
        {
            return parseCalendarEventQueryResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventGetResponse>
    {
        static constexpr std::string_view methodName = "CalendarEvent/get";
        static ParsedEnvelope<CalendarEventGetResponse> parse(std::string_view json)
        {
            return parseCalendarEventGetResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventChangesResponse>
    {
        static constexpr std::string_view methodName = "CalendarEvent/changes";
        static ParsedEnvelope<CalendarEventChangesResponse> parse(std::string_view json)
        {
            return parseCalendarEventChangesResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventSetResponse>
    {
        static constexpr std::string_view methodName = "CalendarEvent/set";
        static ParsedEnvelope<CalendarEventSetResponse> parse(std::string_view json)
        {
            return parseCalendarEventSetResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventNotificationQueryResponse>
    {
        static constexpr std::string_view methodName = "CalendarEventNotification/query";
        static ParsedEnvelope<CalendarEventNotificationQueryResponse> parse(std::string_view json)
        {
            return parseCalendarEventNotificationQueryResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventNotificationGetResponse>
    {
        static constexpr std::string_view methodName = "CalendarEventNotification/get";
        static ParsedEnvelope<CalendarEventNotificationGetResponse> parse(std::string_view json)
        {
            return parseCalendarEventNotificationGetResponse(json);
        }
    };
    template <> struct MethodResponseTraits<CalendarEventNotificationChangesResponse>
    {
        static constexpr std::string_view methodName = "CalendarEventNotification/changes";
        static ParsedEnvelope<CalendarEventNotificationChangesResponse> parse(std::string_view json)
        {
            return parseCalendarEventNotificationChangesResponse(json);
        }
    };
} // namespace javelin::jmap::api
