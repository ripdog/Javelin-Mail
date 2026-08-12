#include "app/DaemonRemoteActionDispatcher.h"

#include "app/DaemonServices.h"
#include "app/UndoApplicationPorts.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchHistoryAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::Undo::id.value:
            return launchAction<actions::Undo>(id, performUndo(false));
        case actions::Redo::id.value:
            return launchAction<actions::Redo>(id, performUndo(true));
        case actions::UndoAcknowledgeRemove::id.value:
            return dispatchDecoded<actions::UndoAcknowledgeRemove>(
                id, command,
                [this, &id](const QString& entryId)
                {
                    return acceptValue<actions::UndoAcknowledgeRemove>(
                        id, m_services.undoCommandPort().acknowledgeAndRemove(entryId));
                });
        case actions::UndoForget::id.value:
            return dispatchDecoded<actions::UndoForget>(
                id, command,
                [this, &id](const QString& entryId)
                {
                    return acceptValue<actions::UndoForget>(
                        id, m_services.undoCommandPort().forget(entryId));
                });
        case actions::UndoSnapshot::id.value:
            return acceptValue<actions::UndoSnapshot>(
                id, std::tuple{m_services.undoCommandPort().state(),
                               m_services.undoCommandPort().entries()});
        default:
            return reject(id, QStringLiteral("The history action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
