#pragma once

#include "app/undo/CalendarPreferencePort.h"
#include "app/undo/HistoryCommandExecutor.h"

namespace javelin::app::undo
{
    class CalendarPreferenceExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit CalendarPreferenceExecutor(CalendarPreferencePort& preferences);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        CalendarPreferencePort& m_preferences;
    };
} // namespace javelin::app::undo
