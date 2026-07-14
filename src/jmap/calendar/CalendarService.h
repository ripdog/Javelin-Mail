#pragma once

#include "jmap/JmapCore.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/CalendarRepository.h"

#include <QCoroTask>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}

namespace javelin::jmap::calendar
{
    enum class CalendarServiceErrorCode
    {
        Cache,
        Capability,
        Permission,
        Transport,
        Authentication,
        Protocol,
        Scheduling,
        StaleState,
        Validation,
    };

    struct CalendarServiceError
    {
        CalendarServiceErrorCode code = CalendarServiceErrorCode::Protocol;
        QString message;
    };

    struct VisibleInterval
    {
        LocalDateTime start;
        LocalDateTime end;
    };

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
    };

    struct CreateEventCommand
    {
        std::string accountId;
        CalendarEvent event;
        std::optional<std::string> ifInState;
    };

    struct UpdateEventCommand
    {
        std::string accountId;
        CalendarEvent event;
        std::optional<std::string> ifInState;
    };

    struct DeleteEventCommand
    {
        std::string accountId;
        std::string eventId;
        std::vector<std::string> calendarIds;
        std::optional<std::string> ifInState;
    };

    using CalendarLoadResult =
        std::variant<std::optional<cache::CalendarWindow>, CalendarServiceError>;
    using CalendarRefreshResult = std::variant<RefreshedRange, CalendarServiceError>;
    using CalendarMutationResult = std::variant<CommittedMutation, CalendarServiceError>;
    using CalendarAccountsResult =
        std::variant<std::vector<cache::CalendarAccount>, CalendarServiceError>;
    using CalendarListResult = std::variant<std::vector<Calendar>, CalendarServiceError>;
    using CalendarPreferenceResult = std::variant<std::monostate, CalendarServiceError>;

    class CalendarService
    {
      public:
        CalendarService(cache::DatabaseConnection& connection,
                        api::JmapMethodTransport& methodTransport);

        [[nodiscard]] CalendarLoadResult loadCached(std::string_view accountId,
                                                    const VisibleInterval& interval,
                                                    const TimeZoneId& displayTimeZone) const;
        [[nodiscard]] CalendarAccountsResult accounts() const;
        [[nodiscard]] CalendarListResult calendars(std::string_view accountId) const;
        [[nodiscard]] CalendarPreferenceResult
        setCalendarVisible(std::string_view accountId, std::string_view calendarId, bool visible);
        [[nodiscard]] QCoro::Task<CalendarRefreshResult> refresh(LiveConnectionSettings settings,
                                                                 std::string ownerAccountId,
                                                                 VisibleInterval interval,
                                                                 TimeZoneId displayTimeZone);
        [[nodiscard]] QCoro::Task<CalendarRefreshResult>
        refreshChanged(LiveConnectionSettings settings, std::string ownerAccountId,
                       VisibleInterval interval, TimeZoneId displayTimeZone);
        [[nodiscard]] QCoro::Task<CalendarMutationResult> create(LiveConnectionSettings settings,
                                                                 std::string ownerAccountId,
                                                                 CreateEventCommand command);
        [[nodiscard]] QCoro::Task<CalendarMutationResult> update(LiveConnectionSettings settings,
                                                                 std::string ownerAccountId,
                                                                 UpdateEventCommand command);
        [[nodiscard]] QCoro::Task<CalendarMutationResult> remove(LiveConnectionSettings settings,
                                                                 std::string ownerAccountId,
                                                                 DeleteEventCommand command);

      private:
        [[nodiscard]] std::uint64_t beginRefresh(std::string_view ownerAccountId);
        [[nodiscard]] bool isCurrentRefresh(std::string_view ownerAccountId,
                                            std::uint64_t generation) const;
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        mutate(LiveConnectionSettings settings, std::string ownerAccountId,
               api::CalendarEventSetRequest request, std::vector<std::string> calendarIds);

        cache::DatabaseConnection& m_connection;
        api::JmapMethodTransport& m_methodTransport;
        std::unordered_map<std::string, std::uint64_t> m_refreshGenerations;
    };
} // namespace javelin::jmap::calendar
