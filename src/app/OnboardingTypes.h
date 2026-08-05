#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace javelin::app
{
    enum class OnboardingFeatureKind : std::uint8_t
    {
        Jmap,
        Mail,
        Sending,
        Contacts,
        Calendars,
        Sieve,
        Push,
        OAuth,
        DynamicClientRegistration,
        OfflineAccess,
    };

    struct OnboardingFeature
    {
        OnboardingFeatureKind kind = OnboardingFeatureKind::Jmap;
        bool available = false;
        QString detail;
    };

    struct AccountDiscoveryRequest
    {
        QString emailAddress;
    };

    struct AccountDiscoveryResult
    {
        bool succeeded = false;
        QString error;
        QString emailAddress;
        QString sessionUrl;
        QString resourceUrl;
        QString authorizationEndpoint;
        QString tokenEndpoint;
        QString registrationEndpoint;
        QString revocationEndpoint = {};
        QString issuer;
        QStringList scopes;
        bool refreshTokensSupported = false;
        std::vector<OnboardingFeature> features;
    };

    struct OAuthStartRequest
    {
        AccountDiscoveryResult discovery;
        QString redirectUri;
    };

    struct OAuthStartResult
    {
        bool succeeded = false;
        QString error;
        QString flowId;
        QString authorizationUrl;
        QString callbackState;
    };

    struct OAuthFinishRequest
    {
        QString flowId;
        QString code;
        QString state;
        QString issuer;
    };

    struct OAuthRefreshRequest
    {
        QString sessionUrl;
        QString tokenEndpoint;
        QString clientId;
        QString refreshToken;
        QString resourceUrl = {};
        QString scope = {};
    };

    enum class OAuthRefreshFailureKind : std::uint8_t
    {
        None,
        Transient,
        ReauthenticationRequired,
    };

    struct OAuthRefreshResult
    {
        bool succeeded = false;
        QString error;
        OAuthRefreshFailureKind failureKind = OAuthRefreshFailureKind::None;
        QString accessToken;
        QString refreshToken;
        QString scope;
        qint64 expiresAtEpochSeconds = 0;
    };

    struct AccountAuthenticationResult
    {
        bool succeeded = false;
        QString error;
        QString sessionUrl;
        QString accessToken;
        QString refreshToken;
        QString tokenEndpoint;
        QString clientId;
        QString issuer = {};
        QString resourceUrl = {};
        QString scope = {};
        QString revocationEndpoint = {};
        qint64 expiresAtEpochSeconds = 0;
        std::vector<OnboardingFeature> features;
    };

    struct OAuthRevocationRequest
    {
        QString revocationEndpoint;
        QString clientId;
        QString accessToken;
        QString refreshToken;
    };

    struct OAuthRevocationResult
    {
        bool attempted = false;
        bool succeeded = false;
        QString error;
    };

    struct ManualAuthenticationRequest
    {
        QString emailAddress;
        QString sessionUrl;
        QString accessToken;
    };
} // namespace javelin::app
