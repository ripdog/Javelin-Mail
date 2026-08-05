#pragma once

#include "jmap/JmapCore.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/calendar/CalendarCommandTypes.h"
#include "jmap/calendar/CalendarReader.h"
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
    struct Session;
} // namespace javelin::jmap::api

namespace javelin::jmap::calendar
{
    class CalendarService final : public CalendarReader
    {
      public:
        CalendarService(cache::DatabaseConnection& connection,
                        api::JmapMethodTransport& methodTransport);

        [[nodiscard]] CalendarLoadResult
        loadCached(std::string_view accountId, const VisibleInterval& interval,
                   const TimeZoneId& displayTimeZone) const override;
        [[nodiscard]] CalendarAccountsResult accounts() const override;
        [[nodiscard]] CalendarListResult calendars(std::string_view accountId) const override;
        [[nodiscard]] CalendarPreferenceResult
        setCalendarVisible(std::string_view accountId, std::string_view calendarId, bool visible);
        [[nodiscard]] QCoro::Task<AuthoritativeCalendarEventResult>
        getAuthoritativeEvent(LiveConnectionSettings settings, std::string ownerAccountId,
                              std::string accountId, std::optional<std::string> eventId,
                              std::string uid);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        setCalendarSubscribed(LiveConnectionSettings settings, std::string ownerAccountId,
                              std::string accountId, std::string calendarId, bool subscribed);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        setDefaultCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                           std::string accountId, std::string calendarId);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        createCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                       CreateCalendarCommand command);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        deleteCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                       DeleteCalendarCommand command);
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
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        mutateCalendar(LiveConnectionSettings settings, api::Session session,
                       api::CalendarSetRequest request, std::optional<Calendar> projectedCalendar,
                       std::optional<std::string> deletedCalendarId);

        cache::DatabaseConnection& m_connection;
        api::JmapMethodTransport& m_methodTransport;
        std::unordered_map<std::string, std::uint64_t> m_refreshGenerations;
    };
} // namespace javelin::jmap::calendar
