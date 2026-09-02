#pragma once

#include "jmap/calendar/CalendarCommandTypes.h"

#include <QCoroTask>

#include <string>

namespace javelin::app
{
    class CalendarReminderMaterializationPort
    {
      public:
        virtual ~CalendarReminderMaterializationPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::calendar::CalendarRefreshResult>
        materializeCalendarReminderHorizon(std::string ownerAccountId,
                                           javelin::jmap::calendar::VisibleInterval interval,
                                           javelin::jmap::calendar::TimeZoneId displayTimeZone) = 0;
    };
} // namespace javelin::app
