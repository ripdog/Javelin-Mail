#pragma once

#include "app/undo/HistoryCommandExecutor.h"
#include "app/undo/SieveHistoryPort.h"

namespace javelin::app::undo
{

    class SieveHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit SieveHistoryExecutor(SieveHistoryPort& mailService);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        SieveHistoryPort& m_mailService;
    };

} // namespace javelin::app::undo
