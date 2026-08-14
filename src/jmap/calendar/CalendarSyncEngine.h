#pragma once

#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/calendar/CalendarCommandTypes.h"

#include <QCoroTask>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}
namespace javelin::jmap::calendar
{
    class CalendarProtocolClient;

    class CalendarSyncEngine
    {
      public:
        CalendarSyncEngine(cache::DatabaseConnection& connection,
                           CalendarProtocolClient& protocolClient);

        [[nodiscard]] QCoro::Task<CalendarRefreshResult> refresh(LiveConnectionSettings settings,
                                                                 std::string ownerAccountId,
                                                                 VisibleInterval interval,
                                                                 TimeZoneId displayTimeZone);
        [[nodiscard]] QCoro::Task<CalendarRefreshResult>
        refreshChanged(LiveConnectionSettings settings, std::string ownerAccountId,
                       VisibleInterval interval, TimeZoneId displayTimeZone);
        void invalidateRefresh(std::string_view ownerAccountId);

      private:
        [[nodiscard]] std::uint64_t beginRefresh(std::string_view ownerAccountId);
        [[nodiscard]] bool isCurrentRefresh(std::string_view ownerAccountId,
                                            std::uint64_t generation) const;

        cache::DatabaseConnection& m_connection;
        CalendarProtocolClient& m_protocolClient;
        std::unordered_map<std::string, std::uint64_t> m_refreshGenerations;
    };
} // namespace javelin::jmap::calendar
