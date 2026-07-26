#pragma once

#include "app/undo/AddressBookHistoryPort.h"
#include "app/undo/HistoryCommandExecutor.h"

namespace javelin::app::undo
{
    class AddressBookHistoryExecutor final : public HistoryCommandExecutor
    {
      public:
        AddressBookHistoryExecutor(AddressBookHistoryPort& addressBooks,
                                   ContactHistoryPort& contacts);

        [[nodiscard]] QCoro::Task<HistoryExecutionResult>
        execute(HistoryEntry entry, HistoryExecutionDirection direction) override;

      private:
        AddressBookHistoryPort& m_addressBooks;
        ContactHistoryPort& m_contacts;
    };
} // namespace javelin::app::undo
