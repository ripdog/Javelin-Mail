#pragma once

#include "jmap/api/LiveConnectionSettings.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/calendar/CalendarCommandTypes.h"

#include <QCoroTask>

#include <optional>
#include <string>

namespace javelin::jmap::api
{
    class JmapMethodTransport;
}
namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::calendar
{
    class CalendarProtocolClient
    {
      public:
        CalendarProtocolClient(cache::DatabaseConnection& connection,
                               api::JmapMethodTransport& methodTransport);

        [[nodiscard]] QCoro::Task<api::MethodCallerResult>
        call(api::ApiRequestContext requestContext, api::RequestBuilder request) const;
        [[nodiscard]] QCoro::Task<AuthoritativeCalendarEventResult>
        getAuthoritativeEvent(LiveConnectionSettings settings, std::string ownerAccountId,
                              std::string accountId, std::optional<std::string> eventId,
                              std::string uid);

      private:
        cache::DatabaseConnection& m_connection;
        api::JmapMethodTransport& m_methodTransport;
    };
} // namespace javelin::jmap::calendar
