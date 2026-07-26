#pragma once

#include "app/undo/HistoryTypes.h"
#include "jmap/OperationError.h"

#include <QCoroTask>

namespace javelin::app::undo
{
    class CalendarPreferencePort
    {
      public:
        virtual ~CalendarPreferencePort() = default;

        [[nodiscard]] virtual std::variant<std::optional<std::string>,
                                           javelin::jmap::OperationError>
        currentCalendarPreference(const CalendarPreferenceHistory& history) const = 0;
        [[nodiscard]] virtual QCoro::Task<std::optional<javelin::jmap::OperationError>>
        applyCalendarPreference(CalendarPreferenceHistory history, std::optional<std::string> value,
                                CommandOrigin origin) = 0;
    };
} // namespace javelin::app::undo
