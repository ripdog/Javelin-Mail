#include "app/RemoteOnboardingPort.h"

#include "app/RemoteActionClient.h"

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
        co_return friendlyResult(co_await m_client.call<AccountAuthenticationResult>(
            javelin::protocol::RemoteActionKind::OnboardingFinishOAuth, request));
    }

    QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
    RemoteOnboardingPort::authenticateManually(ManualAuthenticationRequest request)
    {
        co_return friendlyResult(co_await m_client.call<AccountAuthenticationResult>(
            javelin::protocol::RemoteActionKind::OnboardingAuthenticateManually, request));
    }
} // namespace javelin::app
