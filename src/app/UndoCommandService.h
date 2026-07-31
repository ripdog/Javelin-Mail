#pragma once

#include "app/UndoApplicationPorts.h"

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{

    class UndoCommandService final : public UndoCommandPort
    {
      public:
        explicit UndoCommandService(javelin::app::undo::UndoManager& manager,
                                    QObject* parent = nullptr);

        [[nodiscard]] QCoro::Task<bool> undo() override;
        [[nodiscard]] QCoro::Task<bool> redo() override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        acknowledgeAndRemove(const QString& entryId) override;
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError>
        forget(const QString& entryId) override;
        [[nodiscard]] const undo::HistoryState& state() const override;
        [[nodiscard]] const std::vector<undo::HistoryEntry>& entries() const override;

      private:
        javelin::app::undo::UndoManager& m_manager;
    };

} // namespace javelin::app
