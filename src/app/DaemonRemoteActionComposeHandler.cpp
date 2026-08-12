#include "app/DaemonRemoteActionDispatcher.h"

#include "app/ComposeApplicationPorts.h"
#include "app/DaemonServices.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchComposeAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        const auto hydratedSettings = [this, &id](AccountConnectionSettings settings)
            -> std::variant<AccountConnectionSettings, javelin::protocol::CommandReply>
        {
            auto hydrated = m_connectionSettingsHydrator(std::move(settings));
            if (const auto* error = std::get_if<QString>(&hydrated))
                return reject(id, *error);
            return std::get<AccountConnectionSettings>(std::move(hydrated));
        };

        switch (command.action.value)
        {
        case actions::ComposeOpen::id.value:
            return dispatchDecoded<actions::ComposeOpen>(
                id, command,
                [this, &id,
                 &hydratedSettings](AccountConnectionSettings settings,
                                    javelin::jmap::submission::OpenComposeRequest request)
                {
                    auto hydrated = hydratedSettings(std::move(settings));
                    if (const auto* reply = std::get_if<javelin::protocol::CommandReply>(&hydrated))
                        return *reply;
                    return launchAction<actions::ComposeOpen>(
                        id, m_services.composeCommandPort().open(
                                std::get<AccountConnectionSettings>(std::move(hydrated)),
                                std::move(request)));
                });
        case actions::ComposeLoadSenderIdentities::id.value:
            return dispatchDecoded<actions::ComposeLoadSenderIdentities>(
                id, command,
                [this, &id, &hydratedSettings](AccountConnectionSettings settings,
                                               std::string accountId)
                {
                    auto hydrated = hydratedSettings(std::move(settings));
                    if (const auto* reply = std::get_if<javelin::protocol::CommandReply>(&hydrated))
                        return *reply;
                    return launchAction<actions::ComposeLoadSenderIdentities>(
                        id, m_services.composeCommandPort().loadSenderIdentities(
                                std::get<AccountConnectionSettings>(std::move(hydrated)),
                                std::move(accountId)));
                });
        case actions::ComposeSaveDraft::id.value:
            return dispatchDecoded<actions::ComposeSaveDraft>(
                id, command,
                [this, &id, &hydratedSettings](AccountConnectionSettings settings,
                                               javelin::jmap::submission::DraftSnapshot snapshot)
                {
                    auto hydrated = hydratedSettings(std::move(settings));
                    if (const auto* reply = std::get_if<javelin::protocol::CommandReply>(&hydrated))
                        return *reply;
                    return launchAction<actions::ComposeSaveDraft>(
                        id, m_services.composeCommandPort().saveDraft(
                                std::get<AccountConnectionSettings>(std::move(hydrated)),
                                std::move(snapshot)));
                });
        case actions::ComposeSend::id.value:
            return dispatchDecoded<actions::ComposeSend>(
                id, command,
                [this, &id, &hydratedSettings](AccountConnectionSettings settings,
                                               javelin::jmap::submission::DraftSnapshot snapshot)
                {
                    auto hydrated = hydratedSettings(std::move(settings));
                    if (const auto* reply = std::get_if<javelin::protocol::CommandReply>(&hydrated))
                        return *reply;
                    return launchAction<actions::ComposeSend>(
                        id, m_services.composeCommandPort().send(
                                std::get<AccountConnectionSettings>(std::move(hydrated)),
                                std::move(snapshot)));
                });
        case actions::ComposeScheduleSend::id.value:
            return dispatchDecoded<actions::ComposeScheduleSend>(
                id, command,
                [this, &id,
                 &hydratedSettings](AccountConnectionSettings settings,
                                    javelin::jmap::submission::ScheduledSendRequest request)
                {
                    auto hydrated = hydratedSettings(std::move(settings));
                    if (const auto* reply = std::get_if<javelin::protocol::CommandReply>(&hydrated))
                        return *reply;
                    return launchAction<actions::ComposeScheduleSend>(
                        id, m_services.composeCommandPort().scheduleSend(
                                std::get<AccountConnectionSettings>(std::move(hydrated)),
                                std::move(request)));
                });
        case actions::ComposeLoadWorkingCopy::id.value:
            return dispatchDecoded<actions::ComposeLoadWorkingCopy>(
                id, command,
                [this, &id](const std::string& sessionId)
                {
                    return acceptValue<actions::ComposeLoadWorkingCopy>(
                        id, m_services.composeCommandPort().loadWorkingCopy(sessionId));
                });
        case actions::ComposeStoreWorkingCopy::id.value:
            return dispatchDecoded<actions::ComposeStoreWorkingCopy>(
                id, command,
                [this, &id](const javelin::jmap::submission::DraftSnapshot& snapshot)
                {
                    return acceptValue<actions::ComposeStoreWorkingCopy>(
                        id, m_services.composeCommandPort().storeWorkingCopy(snapshot));
                });
        case actions::ComposeDiscard::id.value:
            return dispatchDecoded<actions::ComposeDiscard>(
                id, command,
                [this, &id](const std::string& sessionId)
                {
                    return acceptValue<actions::ComposeDiscard>(
                        id, m_services.composeCommandPort().discard(sessionId));
                });
        default:
            return reject(id, QStringLiteral("The compose action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
