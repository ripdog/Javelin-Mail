#pragma once

#include "app/undo/HistoryCommandExecutor.h"
#include "app/undo/MailTransferHistoryPort.h"

namespace javelin::app::undo
{
    class MailTransferHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit MailTransferHistoryExecutor(MailTransferHistoryPort& mail);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        MailTransferHistoryPort& m_mail;
    };

} // namespace javelin::app::undo
