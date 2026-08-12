#include "app/RemoteOnboardingPort.h"

#include "app/RemoteActionClient.h"

#include <QLoggingCategory>
#include <QUrl>

Q_LOGGING_CATEGORY(remoteOAuthLog, "javelin.oauth.remote")

namespace javelin::app
{
    namespace
    {
        template <typename Result>
        [[nodiscard]] OnboardingCallResult<Result>
        friendlyResult(DecodedRemoteResult<Result> result)
        {
            if (const auto* error = std::get_if<RemoteCallError>(&result))
                return error->detail;
            return std::get<Result>(std::move(result));
        }
    } // namespace

    RemoteOnboardingPort::RemoteOnboardingPort(RemoteActionClient& client) : m_client(client)
    {
    }

    QCoro::Task<OnboardingCallResult<AccountDiscoveryResult>>
    RemoteOnboardingPort::discover(AccountDiscoveryRequest request)
    {
        co_return friendlyResult(
            co_await m_client.call<javelin::protocol::actions::OnboardingDiscover>(request));
    }

    QCoro::Task<OnboardingCallResult<OAuthStartResult>>
    RemoteOnboardingPort::startOAuth(OAuthStartRequest request)
    {
        co_return friendlyResult(
            co_await m_client.call<javelin::protocol::actions::OnboardingStartOAuth>(request));
    }

    QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
    RemoteOnboardingPort::finishOAuth(OAuthFinishRequest request)
    {
        auto result = friendlyResult(
            co_await m_client.call<javelin::protocol::actions::OnboardingFinishOAuth>(request));
        if (const auto* authentication = std::get_if<AccountAuthenticationResult>(&result))
        {
            qCInfo(remoteOAuthLog).noquote()
                << "OAuth result decoded from daemon"
                << "succeeded=" << authentication->succeeded
                << "credentialHandlePresent=" << !authentication->credentialHandle.isEmpty()
                << "clientIdPresent=" << !authentication->clientId.isEmpty()
                << "tokenEndpointHost=" << QUrl{authentication->tokenEndpoint}.host()
                << "expiresAtEpochSeconds=" << authentication->expiresAtEpochSeconds;
        }
        else
        {
            qCWarning(remoteOAuthLog) << "OAuth daemon call returned an error";
        }
        co_return result;
    }

    QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
    RemoteOnboardingPort::authenticateManually(ManualAuthenticationRequest request)
    {
        co_return friendlyResult(
            co_await m_client.call<javelin::protocol::actions::OnboardingAuthenticateManually>(
                request));
    }

    QCoro::Task<OnboardingCallResult<OAuthRevocationResult>>
    RemoteOnboardingPort::revokeOAuth(OAuthRevocationRequest request)
    {
        co_return friendlyResult(
            co_await m_client.call<javelin::protocol::actions::OnboardingRevokeOAuth>(request));
    }

    QCoro::Task<OnboardingCallResult<OAuthCancelResult>>
    RemoteOnboardingPort::cancelOAuth(OAuthCancelRequest request)
    {
        co_return friendlyResult(
            co_await m_client.call<javelin::protocol::actions::OnboardingCancelOAuth>(request));
    }
} // namespace javelin::app
