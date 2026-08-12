#include "app/DaemonRemoteActionDispatcher.h"

#include "app/DaemonServices.h"
#include "app/IdentityApplicationPorts.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchIdentityAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::IdentityList::id.value:
            return dispatchDecoded<actions::IdentityList>(
                id, command,
                [this, &id](std::string accountId)
                {
                    return launchAction<actions::IdentityList>(
                        id, m_services.identityCommandPort().requestSenderIdentities(
                                std::move(accountId)));
                });
        case actions::IdentitySave::id.value:
            return dispatchDecoded<actions::IdentitySave>(
                id, command,
                [this, &id](std::string accountId, javelin::jmap::domain::Identity identity)
                {
                    return launchAction<actions::IdentitySave>(
                        id, m_services.identityCommandPort().saveSenderIdentity(
                                std::move(accountId), std::move(identity)));
                });
        case actions::IdentityDelete::id.value:
            return dispatchDecoded<actions::IdentityDelete>(
                id, command,
                [this, &id](std::string accountId, std::string identityId)
                {
                    return launchAction<actions::IdentityDelete>(
                        id, m_services.identityCommandPort().deleteSenderIdentity(
                                std::move(accountId), std::move(identityId)));
                });
        default:
            return reject(id, QStringLiteral("The identity action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
