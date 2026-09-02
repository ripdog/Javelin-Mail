#pragma once

#include "jmap/calendar/CalendarTypes.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

    struct CalendarReminderHorizon
    {
        std::string accountId;
        calendar::LocalDateTime start;
        calendar::LocalDateTime end;
        calendar::TimeZoneId displayTimeZone;
        std::string eventState;
        std::vector<calendar::CalendarEvent> events;
        std::vector<calendar::Occurrence> occurrences;
    };

    enum class CalendarEventStatePersistence
    {
        AdvanceCursor,
        PreserveCursor,
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
        [[nodiscard]] std::optional<DatabaseError>
        applyCalendarSubscription(DatabaseTransaction& transaction, std::string_view accountId,
                                  std::string_view calendarId, std::string_view state,
                                  bool subscribed);
        [[nodiscard]] std::optional<DatabaseError>
        applyCalendarColor(DatabaseTransaction& transaction, std::string_view accountId,
                           std::string_view calendarId, std::string_view state,
                           const std::optional<std::string>& color);
        [[nodiscard]] std::optional<DatabaseError> applyCalendarDefaultAlerts(
            DatabaseTransaction& transaction, std::string_view accountId,
            std::string_view calendarId, std::string_view state,
            const std::unordered_map<std::string, calendar::Alert>& withTime,
            const std::unordered_map<std::string, calendar::Alert>& withoutTime);
        [[nodiscard]] std::optional<DatabaseError>
        applyCalendarDefaults(DatabaseTransaction& transaction, std::string_view accountId,
                              std::string_view state,
                              const std::unordered_map<std::string, bool>& defaults);
        [[nodiscard]] std::optional<DatabaseError>
        projectCalendarCreation(DatabaseTransaction& transaction, std::string_view accountId,
                                std::string_view state, const calendar::Calendar& calendar);
        [[nodiscard]] std::optional<DatabaseError>
        projectCalendarDeletion(DatabaseTransaction& transaction, std::string_view accountId,
                                std::string_view calendarId, std::string_view mutationId);
        [[nodiscard]] std::optional<DatabaseError>
        clearCalendarDeletion(DatabaseTransaction& transaction, std::string_view mutationId);
        [[nodiscard]] std::optional<DatabaseError>
        removeProjectedCalendar(DatabaseTransaction& transaction, std::string_view accountId,
                                std::string_view calendarId);
        [[nodiscard]] std::optional<DatabaseError>
        acceptProjectedCalendar(DatabaseTransaction& transaction, std::string_view accountId,
                                std::string_view projectedId, std::string_view acceptedId,
                                std::string_view state, bool isDefault);
        [[nodiscard]] std::variant<std::vector<CalendarAccount>, DatabaseError>
        listAccounts() const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        stateToken(std::string_view accountId, std::string_view dataType) const;
        [[nodiscard]] std::variant<std::optional<calendar::CalendarEvent>, DatabaseError>
        findEvent(std::string_view accountId, std::string_view eventId) const;
        [[nodiscard]] std::variant<std::vector<calendar::Occurrence>, DatabaseError>
        listEventOccurrences(std::string_view accountId, std::string_view eventId) const;
        [[nodiscard]] std::optional<DatabaseError>
        invalidateEventWindows(DatabaseTransaction& transaction, std::string_view accountId,
                               std::span<const std::string> eventIds);
        [[nodiscard]] std::optional<DatabaseError>
        advanceEventWindows(DatabaseTransaction& transaction, std::string_view accountId,
                            std::string_view oldState, std::string_view newState);
        [[nodiscard]] std::optional<DatabaseError> storeStateTokens(std::string_view accountId,
                                                                    std::string_view calendarState,
                                                                    std::string_view eventState);

        [[nodiscard]] std::optional<DatabaseError> reconcileWindow(const CalendarWindow& window);
        [[nodiscard]] std::optional<DatabaseError> reconcileWindow(DatabaseTransaction& transaction,
                                                                   const CalendarWindow& window);
        [[nodiscard]] std::optional<DatabaseError>
        reconcileReminderHorizon(const CalendarReminderHorizon& horizon);
        [[nodiscard]] std::optional<DatabaseError>
        reconcileReminderHorizon(DatabaseTransaction& transaction,
                                 const CalendarReminderHorizon& horizon);
        [[nodiscard]] std::optional<DatabaseError>
        applyEventDelta(std::string_view accountId, std::string_view calendarState,
                        std::string_view eventState, const calendar::TimeZoneId& displayTimeZone,
                        const std::vector<calendar::CalendarEvent>& events,
                        const std::vector<calendar::Occurrence>& occurrences,
                        const std::vector<std::string>& destroyedEventIds);
        [[nodiscard]] std::optional<DatabaseError>
        projectEvents(DatabaseTransaction& transaction, std::string_view accountId,
                      std::string_view eventState,
                      const std::vector<calendar::CalendarEvent>& events,
                      const std::vector<calendar::Occurrence>& replacementOccurrences,
                      std::span<const std::string> destroyedEventIds,
                      CalendarEventStatePersistence statePersistence =
                          CalendarEventStatePersistence::AdvanceCursor);

        [[nodiscard]] std::variant<std::optional<CalendarWindow>, DatabaseError>
        loadWindow(std::string_view accountId, const calendar::LocalDateTime& start,
                   const calendar::LocalDateTime& end,
                   const calendar::TimeZoneId& displayTimeZone) const;
        [[nodiscard]] std::variant<CalendarWindow, DatabaseError>
        loadRangeSnapshot(std::string_view accountId, const calendar::LocalDateTime& start,
                          const calendar::LocalDateTime& end,
                          const calendar::TimeZoneId& displayTimeZone) const;

      private:
        DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::cache
