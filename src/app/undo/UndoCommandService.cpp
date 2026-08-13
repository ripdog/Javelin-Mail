#include "app/UndoCommandService.h"

#include "app/undo/UndoManager.h"

namespace javelin::app
{
    UndoCommandService::UndoCommandService(javelin::app::undo::UndoManager& manager,
                                           QObject* parent)
        : UndoCommandPort(parent), m_manager(manager)
    {
        connect(&m_manager, &undo::UndoManager::historyStateChanged, this,
                &UndoCommandPort::historyStateChanged);
        connect(&m_manager, &undo::UndoManager::executionCompleted, this,
                &UndoCommandPort::executionCompleted);
        connect(&m_manager, &undo::UndoManager::executionFailed, this,
                &UndoCommandPort::executionFailed);
    }

    QCoro::Task<bool> UndoCommandService::undo()
    {
        return m_manager.undo();
    }

    QCoro::Task<bool> UndoCommandService::redo()
    {
        return m_manager.redo();
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    UndoCommandService::acknowledgeAndRemove(const QString& entryId)
    {
        return m_manager.acknowledgeAndRemove(entryId);
    }

    std::optional<javelin::jmap::cache::DatabaseError>
    UndoCommandService::forget(const QString& entryId)
    {
        return m_manager.forget(entryId);
    }

    const undo::HistoryState& UndoCommandService::state() const
    {
        return m_manager.state();
    }

    const std::vector<undo::HistoryEntry>& UndoCommandService::entries() const
    {
        return m_manager.entries();
    }

} // namespace javelin::app
