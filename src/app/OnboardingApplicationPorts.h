#pragma once

#include "app/OnboardingTypes.h"

#include <QCoroTask>

#include <QString>

#include <variant>

namespace javelin::app
{
    template <typename Result> using OnboardingCallResult = std::variant<Result, QString>;

    class OnboardingPort
    {
      public:
        virtual ~OnboardingPort() = default;

        [[nodiscard]] virtual QCoro::Task<OnboardingCallResult<AccountDiscoveryResult>>
        discover(AccountDiscoveryRequest request) = 0;
        [[nodiscard]] virtual QCoro::Task<OnboardingCallResult<OAuthStartResult>>
        startOAuth(OAuthStartRequest request) = 0;
        [[nodiscard]] virtual QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
        finishOAuth(OAuthFinishRequest request) = 0;
        [[nodiscard]] virtual QCoro::Task<OnboardingCallResult<AccountAuthenticationResult>>
        authenticateManually(ManualAuthenticationRequest request) = 0;
    };
} // namespace javelin::app
