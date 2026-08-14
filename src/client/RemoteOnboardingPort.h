#pragma once

#include "app/OnboardingApplicationPorts.h"

namespace javelin::app
{
    class RemoteActionClient;

    class RemoteOnboardingPort final : public OnboardingPort
    {
      public:
        explicit RemoteOnboardingPort(RemoteActionClient& client);

        [[nodiscard]] QCoro::Task<OnboardingCallResult<AccountDiscoveryResult>>
        discover(AccountDiscoveryRequest request) override;
        [[nodiscard]] QCoro::Task<OnboardingCallResult<OAuthStartResult>>
        startOAuth(OAuthStartRequest request) override;
        [[nodiscard]] QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
        finishOAuth(OAuthFinishRequest request) override;
        [[nodiscard]] QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
        authenticateManually(ManualAuthenticationRequest request) override;
        [[nodiscard]] QCoro::Task<OnboardingCallResult<OAuthRevocationResult>>
        revokeOAuth(OAuthRevocationRequest request) override;
        [[nodiscard]] QCoro::Task<OnboardingCallResult<OAuthCancelResult>>
        cancelOAuth(OAuthCancelRequest request) override;

      private:
        RemoteActionClient& m_client;
    };
} // namespace javelin::app
