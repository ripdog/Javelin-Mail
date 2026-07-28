#pragma once

#include "jmap/JmapCore.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/sync/MutationCommitReceipt.h"

#include <QCoroTask>

#include <cstdint>
#include <functional>
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

    using CalendarLoadResult = std::variant<std::optional<cache::CalendarWindow>, OperationError>;
    using CalendarRefreshResult = std::variant<RefreshedRange, OperationError>;
    using CalendarMutationResult = std::variant<CommittedMutation, OperationError>;
    using CalendarAccountsResult =
        std::variant<std::vector<cache::CalendarAccount>, OperationError>;
    using CalendarListResult = std::variant<std::vector<Calendar>, OperationError>;
    using CalendarPreferenceResult = std::variant<std::monostate, OperationError>;
    using AuthoritativeCalendarEventResult =
        std::variant<AuthoritativeCalendarEvent, OperationError>;

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
        [[nodiscard]] QCoro::Task<AuthoritativeCalendarEventResult>
        getAuthoritativeEvent(LiveConnectionSettings settings, std::string ownerAccountId,
                              std::string accountId, std::optional<std::string> eventId,
                              std::string uid);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        setDefaultCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                           std::string accountId, std::string calendarId);
        [[nodiscard]] QCoro::Task<CalendarRefreshResult> refresh(LiveConnectionSettings settings,
                                                                 std::string ownerAccountId,
                                                                 VisibleInterval interval,
                                                                 TimeZoneId displayTimeZone);
        [[nodiscard]] QCoro::Task<CalendarRefreshResult>
        refreshChanged(LiveConnectionSettings settings, std::string ownerAccountId,
                       VisibleInterval interval, TimeZoneId displayTimeZone);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        create(LiveConnectionSettings settings, std::string ownerAccountId,
               CreateEventCommand command, std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        update(LiveConnectionSettings settings, std::string ownerAccountId,
               UpdateEventCommand command, std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        remove(LiveConnectionSettings settings, std::string ownerAccountId,
               DeleteEventCommand command, std::function<void()> projectionCommitted = {});

      private:
        [[nodiscard]] std::uint64_t beginRefresh(std::string_view ownerAccountId);
        [[nodiscard]] bool isCurrentRefresh(std::string_view ownerAccountId,
                                            std::uint64_t generation) const;
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        mutate(LiveConnectionSettings settings, std::string ownerAccountId,
               api::CalendarEventSetRequest request, std::vector<std::string> calendarIds,
               std::optional<std::string> operationGroupId,
               std::optional<CalendarRangeMaterialization> materialization,
               std::function<void()> projectionCommitted);

        cache::DatabaseConnection& m_connection;
        api::JmapMethodTransport& m_methodTransport;
        std::unordered_map<std::string, std::uint64_t> m_refreshGenerations;
    };
} // namespace javelin::jmap::calendar
