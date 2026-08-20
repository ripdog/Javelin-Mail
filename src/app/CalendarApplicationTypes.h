#pragma once

#include "jmap/OperationError.h"

#include <cstddef>
#include <optional>
#include <string>

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
} // namespace javelin::app
