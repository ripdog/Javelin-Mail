#include "app/DaemonRemoteActionDispatcher.h"

#include "app/AccountApplicationPorts.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/DaemonServices.h"
#include "jmap/auth/AccountOnboardingService.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchAccountAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::OnboardingDiscover::id.value:
            return dispatchDecoded<actions::OnboardingDiscover>(
                id, command,
                [this, &id](AccountDiscoveryRequest request)
                {
                    return launchAction<actions::OnboardingDiscover>(
                        id, m_services.onboardingService().discover(std::move(request)));
                });
        case actions::OnboardingStartOAuth::id.value:
            return dispatchDecoded<actions::OnboardingStartOAuth>(
                id, command,
                [this, &id](OAuthStartRequest request)
                {
                    return launchAction<actions::OnboardingStartOAuth>(
                        id, m_services.onboardingService().startOAuth(std::move(request)));
                });
        case actions::OnboardingFinishOAuth::id.value:
            return dispatchDecoded<actions::OnboardingFinishOAuth>(
                id, command,
                [this, &id](OAuthFinishRequest request)
                {
                    return launchAction<actions::OnboardingFinishOAuth>(
                        id, finishOAuthAndFilter(std::move(request)));
                });
        case actions::OnboardingAuthenticateManually::id.value:
            return dispatchDecoded<actions::OnboardingAuthenticateManually>(
                id, command,
                [this, &id](ManualAuthenticationRequest request)
                {
                    return launchAction<actions::OnboardingAuthenticateManually>(
                        id, authenticateManuallyAndFilter(std::move(request)));
                });
        case actions::OnboardingRevokeOAuth::id.value:
            return dispatchDecoded<actions::OnboardingRevokeOAuth>(
                id, command,
                [this, &id](OAuthRevocationRequest request)
                {
                    auto hydrated = m_revocationRequestHydrator(std::move(request));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    return launchAction<actions::OnboardingRevokeOAuth>(
                        id, m_services.onboardingService().revokeOAuth(
                                std::get<OAuthRevocationRequest>(std::move(hydrated))));
                });
        case actions::OnboardingCancelOAuth::id.value:
            return dispatchDecoded<actions::OnboardingCancelOAuth>(
                id, command,
                [this, &id](OAuthCancelRequest request)
                {
                    return launchAction<actions::OnboardingCancelOAuth>(
                        id, m_services.onboardingService().cancelOAuth(std::move(request)));
                });
        case actions::RemoveConfiguredAccount::id.value:
            return dispatchDecoded<actions::RemoveConfiguredAccount>(
                id, command,
                [this, &id](const QString& loginEmail, const QString& sessionUrl,
                            const QStringList& accountIds)
                {
                    return acceptValue<actions::RemoveConfiguredAccount>(
                        id, m_services.accountCommandPort().removeConfiguredAccount(
                                loginEmail, sessionUrl, accountIds));
                });
        case actions::AccountBootstrap::id.value:
            return dispatchDecoded<actions::AccountBootstrap>(
                id, command,
                [this, &id](AccountBootstrapIntent intent)
                {
                    auto hydrated = m_connectionSettingsHydrator(std::move(intent.settings));
                    if (const auto* error = std::get_if<QString>(&hydrated))
                        return reject(id, *error);
                    intent.settings = std::get<AccountConnectionSettings>(std::move(hydrated));
                    return launchAction<actions::AccountBootstrap>(
                        id, m_services.accountRefreshPort().bootstrapAccount(std::move(intent)));
                });
        case actions::ReloadSettings::id.value:
            if (const auto error = m_reloadSettings())
                return reject(id, error->detail, error->code);
            return acceptEmpty<actions::ReloadSettings>(id);
        default:
            return reject(id, QStringLiteral("The account action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
