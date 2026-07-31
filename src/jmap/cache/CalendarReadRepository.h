#pragma once

#include "jmap/cache/CalendarRepository.h"

namespace javelin::jmap::cache
{

    class CalendarReadRepository final
    {
      public:
        explicit CalendarReadRepository(const DatabaseReadView& connection);

        [[nodiscard]] std::variant<std::vector<calendar::Calendar>, DatabaseError>
        listCalendars(std::string_view accountId) const;
        [[nodiscard]] std::variant<std::vector<CalendarAccount>, DatabaseError>
        listAccounts() const;
        [[nodiscard]] std::variant<std::optional<std::string>, DatabaseError>
        stateToken(std::string_view accountId, std::string_view dataType) const;
        [[nodiscard]] std::variant<std::optional<calendar::CalendarEvent>, DatabaseError>
        findEvent(std::string_view accountId, std::string_view eventId) const;
        [[nodiscard]] std::variant<std::optional<CalendarWindow>, DatabaseError>
        loadWindow(std::string_view accountId, const calendar::LocalDateTime& start,
                   const calendar::LocalDateTime& end,
                   const calendar::TimeZoneId& displayTimeZone) const;

      private:
        DatabaseReadView m_connection;
    };

} // namespace javelin::jmap::cache
