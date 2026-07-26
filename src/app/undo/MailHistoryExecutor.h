#pragma once

#include "app/undo/HistoryCommandExecutor.h"
#include "app/undo/MailHistoryPort.h"

namespace javelin::app::undo
{

    class MailHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit MailHistoryExecutor(MailHistoryPort& mailService);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        MailHistoryPort& m_mailService;
    };

} // namespace javelin::app::undo
