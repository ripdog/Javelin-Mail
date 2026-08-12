#include "app/DaemonRemoteActionDispatcher.h"

#include "app/DaemonServices.h"
#include "app/SieveApplicationPorts.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchSieveAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::SieveList::id.value:
            return dispatchDecoded<actions::SieveList>(
                id, command,
                [this, &id](std::string accountId)
                {
                    return launchAction<actions::SieveList>(
                        id,
                        m_services.sieveCommandPort().requestSieveScripts(std::move(accountId)));
                });
        case actions::SieveGet::id.value:
            return dispatchDecoded<actions::SieveGet>(
                id, command,
                [this, &id](std::string accountId, javelin::jmap::sieve::SieveScript script)
                {
                    return launchAction<actions::SieveGet>(
                        id, m_services.sieveCommandPort().requestSieveScript(std::move(accountId),
                                                                             std::move(script)));
                });
        case actions::SieveValidate::id.value:
            return dispatchDecoded<actions::SieveValidate>(
                id, command,
                [this, &id](std::string accountId, QByteArray content)
                {
                    return launchAction<actions::SieveValidate>(
                        id, m_services.sieveCommandPort().validateSieveScript(std::move(accountId),
                                                                              std::move(content)));
                });
        case actions::SieveSave::id.value:
            return dispatchDecoded<actions::SieveSave>(
                id, command,
                [this, &id](std::string accountId, javelin::jmap::sieve::SieveScript script,
                            QByteArray content, const undo::CommandOrigin origin)
                {
                    return launchAction<actions::SieveSave>(
                        id,
                        m_services.sieveCommandPort().saveSieveScript(
                            std::move(accountId), std::move(script), std::move(content), origin));
                });
        case actions::SieveDelete::id.value:
            return dispatchDecoded<actions::SieveDelete>(
                id, command,
                [this, &id](std::string accountId, javelin::jmap::sieve::SieveScript script,
                            const undo::CommandOrigin origin)
                {
                    return launchAction<actions::SieveDelete>(
                        id, m_services.sieveCommandPort().deleteSieveScript(
                                std::move(accountId), std::move(script), origin));
                });
        case actions::SieveActivate::id.value:
            return dispatchDecoded<actions::SieveActivate>(
                id, command,
                [this, &id](std::string accountId, javelin::jmap::sieve::SieveScript script,
                            const bool active, const undo::CommandOrigin origin)
                {
                    return launchAction<actions::SieveActivate>(
                        id, m_services.sieveCommandPort().setSieveScriptActive(
                                std::move(accountId), std::move(script), active, origin));
                });
        default:
            return reject(id, QStringLiteral("The Sieve action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
