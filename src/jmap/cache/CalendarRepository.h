#pragma once

#include "jmap/cache/Database.h"
#include "jmap/calendar/CalendarTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    struct CalendarWindow
    {
        std::string accountId;
        calendar::LocalDateTime start;
        calendar::LocalDateTime end;
        calendar::TimeZoneId displayTimeZone;
        std::string queryState;
        std::string eventState;
        std::vector<calendar::CalendarEvent> events;
        std::vector<calendar::Occurrence> occurrences;
    };

    struct CalendarAccount
    {
        std::string ownerAccountId;
        std::string accountId;
        std::string name;
    };

    class CalendarRepository
    {
      public:
        explicit CalendarRepository(DatabaseConnection& connection);

        [[nodiscard]] std::optional<DatabaseError>
        replaceCalendars(std::string_view accountId, std::string_view state,
                         const std::vector<calendar::Calendar>& calendars);

        [[nodiscard]] std::variant<std::vector<calendar::Calendar>, DatabaseError>
        listCalendars(std::string_view accountId) const;
        [[nodiscard]] std::optional<DatabaseError>
        setCalendarVisible(std::string_view accountId, std::string_view calendarId, bool visible);
        [[nodiscard]] std::optional<DatabaseError> setDefaultCalendar(std::string_view accountId,
                                                                      std::string_view calendarId);
        [[nodiscard]] std::variant<std::vector<CalendarAccount>, DatabaseError>
        listAccounts() const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        stateToken(std::string_view accountId, std::string_view dataType) const;
        [[nodiscard]] std::optional<DatabaseError> storeStateTokens(std::string_view accountId,
                                                                    std::string_view calendarState,
                                                                    std::string_view eventState);

        [[nodiscard]] std::optional<DatabaseError> reconcileWindow(const CalendarWindow& window);
        [[nodiscard]] std::optional<DatabaseError>
        applyEventDelta(std::string_view accountId, std::string_view calendarState,
                        std::string_view eventState, const calendar::TimeZoneId& displayTimeZone,
                        const std::vector<calendar::CalendarEvent>& events,
                        const std::vector<calendar::Occurrence>& occurrences,
                        const std::vector<std::string>& destroyedEventIds);

        [[nodiscard]] std::variant<std::optional<CalendarWindow>, DatabaseError>
        loadWindow(std::string_view accountId, const calendar::LocalDateTime& start,
                   const calendar::LocalDateTime& end,
                   const calendar::TimeZoneId& displayTimeZone) const;

      private:
        DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::cache
