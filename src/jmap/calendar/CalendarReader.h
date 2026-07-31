#pragma once

#include "jmap/OperationError.h"
#include "jmap/cache/CalendarRepository.h"
#include "jmap/calendar/CalendarTypes.h"

#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::calendar
{

    struct VisibleInterval
    {
        LocalDateTime start;
        LocalDateTime end;
    };

    using CalendarLoadResult = std::variant<std::optional<cache::CalendarWindow>, OperationError>;
    using CalendarAccountsResult =
        std::variant<std::vector<cache::CalendarAccount>, OperationError>;
    using CalendarListResult = std::variant<std::vector<Calendar>, OperationError>;

    class CalendarReader
    {
      public:
        virtual ~CalendarReader() = default;

        [[nodiscard]] virtual CalendarLoadResult
        loadCached(std::string_view accountId, const VisibleInterval& interval,
                   const TimeZoneId& displayTimeZone) const = 0;
        [[nodiscard]] virtual CalendarAccountsResult accounts() const = 0;
        [[nodiscard]] virtual CalendarListResult calendars(std::string_view accountId) const = 0;
    };

} // namespace javelin::jmap::calendar
