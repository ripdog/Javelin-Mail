#pragma once

#include "jmap/calendar/CalendarReader.h"

#include <QString>

#include <cstddef>

namespace javelin::app
{

    struct CalendarCacheChange
    {
        QString ownerAccountId;
        javelin::jmap::calendar::VisibleInterval interval;
        javelin::jmap::calendar::TimeZoneId displayTimeZone;
        std::size_t accountCount = 0;
        std::size_t eventCount = 0;
    };

} // namespace javelin::app
