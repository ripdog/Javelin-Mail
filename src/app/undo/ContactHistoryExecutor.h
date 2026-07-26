#pragma once

#include "app/undo/ContactHistoryPort.h"
#include "app/undo/HistoryCommandExecutor.h"

namespace javelin::app::undo
{
    class ContactHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        explicit ContactHistoryExecutor(ContactHistoryPort& contacts);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        ContactHistoryPort& m_contacts;
    };
} // namespace javelin::app::undo
