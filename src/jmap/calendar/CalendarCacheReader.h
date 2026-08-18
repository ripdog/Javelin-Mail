#pragma once

#include "jmap/calendar/CalendarReader.h"

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::calendar
{
    class CalendarCacheReader final : public CalendarReader
    {
      public:
        explicit CalendarCacheReader(cache::DatabaseConnection& connection);

        [[nodiscard]] CalendarLoadResult
        loadCached(std::string_view accountId, const VisibleInterval& interval,
                   const TimeZoneId& displayTimeZone) const override;
        [[nodiscard]] CalendarAccountsResult accounts() const override;
        [[nodiscard]] CalendarListResult calendars(std::string_view accountId) const override;
        [[nodiscard]] ParticipantIdentityListResult
        participantIdentities(std::string_view accountId) const override;
        [[nodiscard]] PendingCalendarInvitationsResult pendingInvitations() const override;
        [[nodiscard]] CalendarEventReadResult event(std::string_view accountId,
                                                    std::string_view eventId) const override;

      private:
        cache::DatabaseConnection& m_connection;
    };
} // namespace javelin::jmap::calendar
