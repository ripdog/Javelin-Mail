#pragma once

#include "jmap/OperationError.h"
#include "jmap/calendar/CalendarTypes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::app
{
    struct CalendarColorChange
    {
        std::string ownerAccountId;
        std::string accountId;
        std::string calendarId;
        std::optional<std::string> color;
    };

    struct CalendarColorFailure
    {
        CalendarColorChange change;
        javelin::jmap::OperationError error;
    };

    struct CalendarColorBatchResult
    {
        std::size_t requestedCount = 0;
        std::size_t appliedCount = 0;
        std::vector<CalendarColorFailure> failures;
        std::optional<javelin::jmap::OperationError> error;
    };

    struct CalendarDefaultAlertsChange
    {
        std::string ownerAccountId;
        std::string accountId;
        std::string calendarId;
        std::unordered_map<std::string, javelin::jmap::calendar::Alert> withTime;
        std::unordered_map<std::string, javelin::jmap::calendar::Alert> withoutTime;
    };

    struct CalendarDefaultAlertsFailure
    {
        CalendarDefaultAlertsChange change;
        javelin::jmap::OperationError error;
    };

    struct CalendarDefaultAlertsBatchResult
    {
        std::size_t requestedCount = 0;
        std::size_t appliedCount = 0;
        std::vector<CalendarDefaultAlertsFailure> failures;
        std::optional<javelin::jmap::OperationError> error;
    };
} // namespace javelin::app
