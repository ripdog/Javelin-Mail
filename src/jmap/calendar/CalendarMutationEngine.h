#pragma once

#include "jmap/api/CalendarMethods.h"
#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/calendar/CalendarCommandTypes.h"

#include <QCoroTask>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::api
{
    struct Session;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::calendar
{
    class CalendarProtocolClient;
    class CalendarReader;
    class CalendarSyncEngine;

    class CalendarMutationEngine
    {
      public:
        CalendarMutationEngine(cache::DatabaseConnection& connection,
                               CalendarProtocolClient& protocolClient,
                               CalendarSyncEngine& syncEngine, CalendarReader& reader);

        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        setCalendarSubscribed(LiveConnectionSettings settings, std::string ownerAccountId,
                              std::string accountId, std::string calendarId, bool subscribed);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        setDefaultCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                           std::string accountId, std::string calendarId);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        setCalendarColor(LiveConnectionSettings settings, std::string ownerAccountId,
                         std::string accountId, std::string calendarId,
                         std::optional<std::string> color);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        createCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                       CreateCalendarCommand command);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        deleteCalendar(LiveConnectionSettings settings, std::string ownerAccountId,
                       DeleteCalendarCommand command);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        create(LiveConnectionSettings settings, std::string ownerAccountId,
               CreateEventCommand command, std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        update(LiveConnectionSettings settings, std::string ownerAccountId,
               UpdateEventCommand command, std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        remove(LiveConnectionSettings settings, std::string ownerAccountId,
               DeleteEventCommand command, std::function<void()> projectionCommitted = {});
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        respond(LiveConnectionSettings settings, std::string ownerAccountId,
                RespondToEventCommand command, std::function<void()> projectionCommitted = {});

      private:
        enum class MutationPermission
        {
            Write,
            Private,
            Rsvp,
        };

        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        mutate(LiveConnectionSettings settings, std::string ownerAccountId,
               api::CalendarEventSetRequest request, std::vector<std::string> calendarIds,
               std::optional<std::string> operationGroupId,
               std::optional<CalendarRangeMaterialization> materialization,
               MutationPermission permission, std::function<void()> projectionCommitted);
        [[nodiscard]] QCoro::Task<CalendarMutationResult>
        mutateCalendar(LiveConnectionSettings settings, api::Session session,
                       api::CalendarSetRequest request, std::optional<Calendar> projectedCalendar,
                       std::optional<std::string> deletedCalendarId);

        cache::DatabaseConnection& m_connection;
        CalendarProtocolClient& m_protocolClient;
        CalendarSyncEngine& m_syncEngine;
        CalendarReader& m_reader;
    };
} // namespace javelin::jmap::calendar
