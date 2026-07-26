#pragma once

#include "app/undo/DraftHistoryPort.h"
#include "app/undo/HistoryCommandExecutor.h"

namespace javelin::app::undo
{

    class DraftHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit DraftHistoryExecutor(DraftHistoryPort& composeService);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        DraftHistoryPort& m_composeService;
    };

} // namespace javelin::app::undo
