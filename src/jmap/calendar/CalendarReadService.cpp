#include "jmap/calendar/CalendarReadService.h"

namespace javelin::jmap::calendar
{
    CalendarReadService::CalendarReadService(cache::ReadOnlyDatabaseConnection& connection)
        : m_repository(cache::DatabaseReadView{connection})
    {
    }

    CalendarLoadResult CalendarReadService::loadCached(const std::string_view accountId,
                                                       const VisibleInterval& interval,
                                                       const TimeZoneId& displayTimeZone) const
    {
        auto loaded =
            m_repository.loadWindow(accountId, interval.start, interval.end, displayTimeZone);
        if (const auto* error = std::get_if<cache::DatabaseError>(&loaded))
            return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                  .message = error->message};
        return std::get<std::optional<cache::CalendarWindow>>(std::move(loaded));
    }

    CalendarAccountsResult CalendarReadService::accounts() const
    {
        auto loaded = m_repository.listAccounts();
        if (const auto* error = std::get_if<cache::DatabaseError>(&loaded))
            return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                  .message = error->message};
        return std::get<std::vector<cache::CalendarAccount>>(std::move(loaded));
    }

    CalendarListResult CalendarReadService::calendars(const std::string_view accountId) const
    {
        auto loaded = m_repository.listCalendars(accountId);
        if (const auto* error = std::get_if<cache::DatabaseError>(&loaded))
            return OperationError{.code = OperationErrorCode::LocalStorageFailure,
                                  .message = error->message};
        return std::get<std::vector<Calendar>>(std::move(loaded));
    }
} // namespace javelin::jmap::calendar
