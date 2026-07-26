#pragma once

#include "app/undo/CalendarHistoryPort.h"
#include "app/undo/HistoryCommandExecutor.h"

namespace javelin::app::undo
{
    class CalendarHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit CalendarHistoryExecutor(CalendarHistoryPort& calendarService);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        CalendarHistoryPort& m_calendarService;
    };
} // namespace javelin::app::undo
