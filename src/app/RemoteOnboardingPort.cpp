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
        co_return friendlyResult(co_await m_client.call<AccountDiscoveryResult>(
            javelin::protocol::RemoteActionKind::OnboardingDiscover, request));
    }

    QCoro::Task<OnboardingCallResult<OAuthStartResult>>
    RemoteOnboardingPort::startOAuth(OAuthStartRequest request)
    {
        co_return friendlyResult(co_await m_client.call<OAuthStartResult>(
            javelin::protocol::RemoteActionKind::OnboardingStartOAuth, request));
    }

    QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
    RemoteOnboardingPort::finishOAuth(OAuthFinishRequest request)
    {
        auto result = friendlyResult(co_await m_client.call<AccountAuthenticationResult>(
            javelin::protocol::RemoteActionKind::OnboardingFinishOAuth, request));
        if (const auto* authentication = std::get_if<AccountAuthenticationResult>(&result))
        {
            qCInfo(remoteOAuthLog).noquote()
                << "OAuth result decoded from daemon"
                << "succeeded=" << authentication->succeeded
                << "accessTokenPresent=" << !authentication->accessToken.isEmpty()
                << "refreshTokenPresent=" << !authentication->refreshToken.isEmpty()
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
        co_return friendlyResult(co_await m_client.call<AccountAuthenticationResult>(
            javelin::protocol::RemoteActionKind::OnboardingAuthenticateManually, request));
    }

    QCoro::Task<OnboardingCallResult<OAuthRevocationResult>>
    RemoteOnboardingPort::revokeOAuth(OAuthRevocationRequest request)
    {
        co_return friendlyResult(co_await m_client.call<OAuthRevocationResult>(
            javelin::protocol::RemoteActionKind::OnboardingRevokeOAuth, request));
    }
} // namespace javelin::app
