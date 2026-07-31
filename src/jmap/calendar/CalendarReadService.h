#pragma once

#include "jmap/cache/CalendarReadRepository.h"
#include "jmap/calendar/CalendarReader.h"

namespace javelin::jmap::calendar
{

    class CalendarReadService final : public CalendarReader
    {
      public:
        explicit CalendarReadService(cache::ReadOnlyDatabaseConnection& connection);

        [[nodiscard]] CalendarLoadResult
        loadCached(std::string_view accountId, const VisibleInterval& interval,
                   const TimeZoneId& displayTimeZone) const override;
        [[nodiscard]] CalendarAccountsResult accounts() const override;
        [[nodiscard]] CalendarListResult calendars(std::string_view accountId) const override;

      private:
        cache::CalendarReadRepository m_repository;
    };

} // namespace javelin::jmap::calendar
