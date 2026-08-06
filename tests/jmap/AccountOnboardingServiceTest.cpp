#include "jmap/auth/AccountOnboardingService.h"
#include "app/RemoteCodec.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QNetworkAccessManager>

#include <catch2/catch_test_macros.hpp>

#include <variant>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;

        static int argc = 1;
        static char appName[] = "javelin-onboarding-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }
} // namespace

TEST_CASE("manual onboarding refuses to send a token over an insecure connection",
          "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.authenticateManually({
        .emailAddress = QStringLiteral("alice@example.com"),
        .sessionUrl = QStringLiteral("http://mail.example.com/.well-known/jmap"),
        .accessToken = QStringLiteral("secret-token"),
    }));

    CHECK_FALSE(result.succeeded);
    CHECK(result.error == QStringLiteral("Use an HTTPS address for the JMAP server."));
}

TEST_CASE("OAuth refresh refuses an insecure token endpoint", "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.refreshOAuth({
        .sessionUrl = QStringLiteral("https://mail.example.com/.well-known/jmap"),
        .tokenEndpoint = QStringLiteral("http://auth.example.com/token"),
        .clientId = QStringLiteral("javelin"),
        .refreshToken = QStringLiteral("refresh-token"),
    }));

    CHECK_FALSE(result.succeeded);
    CHECK(result.error == QStringLiteral("OAuth refresh information is incomplete."));
    CHECK(result.failureKind == javelin::app::OAuthRefreshFailureKind::ReauthenticationRequired);
}

TEST_CASE("OAuth refresh accepts legacy grants without a resource indicator",
          "[jmap][auth][onboarding]")
{
    javelin::app::OAuthRefreshRequest request{
        .sessionUrl = QStringLiteral("https://mail.example.com/.well-known/jmap"),
        .tokenEndpoint = QStringLiteral("https://auth.example.com/token"),
        .clientId = QStringLiteral("javelin"),
        .refreshToken = QStringLiteral("refresh-token"),
        .resourceUrl = {},
        .scope = {},
    };

    CHECK(javelin::jmap::auth::detail::isUsableOAuthRefreshRequest(request));

    request.resourceUrl = QStringLiteral("http://mail.example.com/jmap");
    CHECK_FALSE(javelin::jmap::auth::detail::isUsableOAuthRefreshRequest(request));
}

TEST_CASE("OAuth refresh errors distinguish expired grants from transient failures",
          "[jmap][auth][onboarding]")
{
    using Kind = javelin::app::OAuthRefreshFailureKind;
    using javelin::jmap::auth::detail::refreshFailureKind;

    CHECK(refreshFailureKind(QStringLiteral("invalid_grant")) == Kind::ReauthenticationRequired);
    CHECK(refreshFailureKind(QStringLiteral("invalid_client")) == Kind::ReauthenticationRequired);
    CHECK(refreshFailureKind(QStringLiteral("temporarily_unavailable")) == Kind::Transient);
    CHECK(refreshFailureKind(QString{}) == Kind::Transient);
}

TEST_CASE("OAuth revocation reports missing endpoint for stored tokens", "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.revokeOAuth({
        .revocationEndpoint = {},
        .clientId = QStringLiteral("javelin"),
        .accessToken = QStringLiteral("access-token"),
        .refreshToken = QStringLiteral("refresh-token"),
    }));

    CHECK(result.attempted);
    CHECK_FALSE(result.succeeded);
    CHECK(result.error == QStringLiteral("OAuth revocation information is incomplete."));
}

TEST_CASE("OAuth revocation skips accounts without OAuth credentials", "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.revokeOAuth({
        .revocationEndpoint = {},
        .clientId = {},
        .accessToken = {},
        .refreshToken = {},
    }));

    CHECK_FALSE(result.attempted);
    CHECK(result.succeeded);
    CHECK(result.error.isEmpty());
}

TEST_CASE("OAuth revocation refuses an insecure endpoint", "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result = QCoro::waitFor(service.revokeOAuth({
        .revocationEndpoint = QStringLiteral("http://auth.example.com/revoke"),
        .clientId = QStringLiteral("javelin"),
        .accessToken = QStringLiteral("access-token"),
        .refreshToken = QStringLiteral("refresh-token"),
    }));

    CHECK(result.attempted);
    CHECK_FALSE(result.succeeded);
    CHECK(result.error == QStringLiteral("OAuth revocation information is incomplete."));
}

TEST_CASE("OAuth revocation request survives the daemon boundary", "[jmap][auth][onboarding]")
{
    const javelin::app::OAuthRevocationRequest request{
        .revocationEndpoint = QStringLiteral("https://auth.example.com/revoke"),
        .clientId = QStringLiteral("client-id"),
        .accessToken = QStringLiteral("access-token"),
        .refreshToken = QStringLiteral("refresh-token"),
        .registrationClientUri = QStringLiteral("https://auth.example.com/register/client-id"),
        .registrationAccessToken = QStringLiteral("registration-token"),
    };

    const auto encoded = javelin::app::remote::encode(request);
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto decoded = javelin::app::remote::decodeValue<javelin::app::OAuthRevocationRequest>(
        std::get<QByteArray>(encoded));
    REQUIRE(std::holds_alternative<javelin::app::OAuthRevocationRequest>(decoded));
    const auto& restored = std::get<javelin::app::OAuthRevocationRequest>(decoded);
    CHECK(restored.revocationEndpoint == request.revocationEndpoint);
    CHECK(restored.clientId == request.clientId);
    CHECK(restored.accessToken == request.accessToken);
    CHECK(restored.refreshToken == request.refreshToken);
    CHECK(restored.registrationClientUri == request.registrationClientUri);
    CHECK(restored.registrationAccessToken == request.registrationAccessToken);
}

TEST_CASE("OAuth cancellation request survives the daemon boundary", "[jmap][auth][onboarding]")
{
    const javelin::app::OAuthCancelRequest request{.flowId = QStringLiteral("flow-id")};

    const auto encoded = javelin::app::remote::encode(request);
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto decoded = javelin::app::remote::decodeValue<javelin::app::OAuthCancelRequest>(
        std::get<QByteArray>(encoded));
    REQUIRE(std::holds_alternative<javelin::app::OAuthCancelRequest>(decoded));
    CHECK(std::get<javelin::app::OAuthCancelRequest>(decoded).flowId == request.flowId);
}

TEST_CASE("OAuth cancellation ignores an unknown flow", "[jmap][auth][onboarding]")
{
    ensureApplication();
    QNetworkAccessManager networkAccessManager;
    javelin::jmap::auth::AccountOnboardingService service{networkAccessManager};

    const auto result =
        QCoro::waitFor(service.cancelOAuth({.flowId = QStringLiteral("missing-flow")}));

    CHECK_FALSE(result.registrationDeleted);
    CHECK(result.error.isEmpty());
}

TEST_CASE("OAuth metadata URLs require secure endpoint syntax", "[jmap][auth][onboarding]")
{
    using javelin::jmap::auth::detail::isSecureOAuthUrl;

    CHECK(isSecureOAuthUrl(QUrl{QStringLiteral("https://auth.example.com/token")}));
    CHECK(
        isSecureOAuthUrl(QUrl{QStringLiteral("https://auth.example.com/authorize?prompt=login")}));
    CHECK_FALSE(isSecureOAuthUrl(QUrl{QStringLiteral("http://auth.example.com/token")}));
    CHECK_FALSE(isSecureOAuthUrl(QUrl{QStringLiteral("https://user@auth.example.com/token")}));
    CHECK_FALSE(isSecureOAuthUrl(QUrl{QStringLiteral("https://auth.example.com/token#fragment")}));
}

TEST_CASE("OAuth protected-resource metadata uses exact resource identifiers",
          "[jmap][auth][onboarding]")
{
    using javelin::jmap::auth::detail::resourceMetadataMatches;

    CHECK(resourceMetadataMatches(QStringLiteral("https://mail.example.com/jmap"),
                                  QStringLiteral("https://mail.example.com/jmap")));
    CHECK_FALSE(resourceMetadataMatches(QStringLiteral("https://mail.example.com"),
                                        QStringLiteral("https://mail.example.com/jmap")));
    CHECK_FALSE(resourceMetadataMatches(QStringLiteral("https://mail.example.com/jmap/"),
                                        QStringLiteral("https://mail.example.com/jmap")));
}

TEST_CASE("OAuth dynamic registration preserves exact loopback callback addresses",
          "[jmap][auth][onboarding]")
{
    using javelin::jmap::auth::detail::registrationRedirectUri;

    CHECK(registrationRedirectUri(QStringLiteral("http://127.0.0.1:49152/oauth/callback")) ==
          QStringLiteral("http://127.0.0.1:49152/oauth/callback"));
    CHECK(registrationRedirectUri(QStringLiteral("http://[::1]:49152/oauth/callback")) ==
          QStringLiteral("http://[::1]:49152/oauth/callback"));
}

TEST_CASE("OAuth start callback identity survives the daemon boundary", "[jmap][auth][onboarding]")
{
    const javelin::app::OAuthStartResult result{
        .succeeded = true,
        .error = {},
        .flowId = QStringLiteral("flow-id"),
        .authorizationUrl = QStringLiteral("https://auth.example.com/authorize"),
        .callbackState = QStringLiteral("callback-state"),
    };

    const auto encoded = javelin::app::remote::encode(result);
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto decoded = javelin::app::remote::decodeValue<javelin::app::OAuthStartResult>(
        std::get<QByteArray>(encoded));
    REQUIRE(std::holds_alternative<javelin::app::OAuthStartResult>(decoded));
    const auto& restored = std::get<javelin::app::OAuthStartResult>(decoded);
    CHECK(restored.flowId == result.flowId);
    CHECK(restored.authorizationUrl == result.authorizationUrl);
    CHECK(restored.callbackState == result.callbackState);
}

TEST_CASE("OAuth discovery resource survives the daemon boundary", "[jmap][auth][onboarding]")
{
    const javelin::app::AccountDiscoveryResult result{
        .succeeded = true,
        .error = {},
        .emailAddress = QStringLiteral("alice@example.com"),
        .sessionUrl = QStringLiteral("https://mail.example.com/.well-known/jmap"),
        .resourceUrl = QStringLiteral("https://mail.example.com/jmap/session"),
        .authorizationEndpoint = QStringLiteral("https://auth.example.com/authorize"),
        .tokenEndpoint = QStringLiteral("https://auth.example.com/token"),
        .registrationEndpoint = QStringLiteral("https://auth.example.com/register"),
        .revocationEndpoint = QStringLiteral("https://auth.example.com/revoke"),
        .issuer = QStringLiteral("https://auth.example.com"),
        .scopes = {QStringLiteral("urn:ietf:params:oauth:scope:mail")},
        .refreshTokensSupported = true,
        .features = {},
    };

    const auto encoded = javelin::app::remote::encode(result);
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto decoded = javelin::app::remote::decodeValue<javelin::app::AccountDiscoveryResult>(
        std::get<QByteArray>(encoded));
    REQUIRE(std::holds_alternative<javelin::app::AccountDiscoveryResult>(decoded));
    const auto& restored = std::get<javelin::app::AccountDiscoveryResult>(decoded);
    CHECK(restored.resourceUrl == result.resourceUrl);
    CHECK(restored.revocationEndpoint == result.revocationEndpoint);
}

TEST_CASE("OAuth browser callback identity survives the daemon boundary",
          "[jmap][auth][onboarding]")
{
    const javelin::app::OAuthFinishRequest request{
        .flowId = QStringLiteral("flow-id"),
        .code = QStringLiteral("authorization-code"),
        .state = QStringLiteral("state"),
        .issuer = QStringLiteral("https://mail.example.com"),
    };

    const auto encoded = javelin::app::remote::encode(request);
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto decoded = javelin::app::remote::decodeValue<javelin::app::OAuthFinishRequest>(
        std::get<QByteArray>(encoded));
    REQUIRE(std::holds_alternative<javelin::app::OAuthFinishRequest>(decoded));
    const auto& restored = std::get<javelin::app::OAuthFinishRequest>(decoded);
    CHECK(restored.flowId == request.flowId);
    CHECK(restored.code == request.code);
    CHECK(restored.state == request.state);
    CHECK(restored.issuer == request.issuer);
}

TEST_CASE("OAuth authentication credentials survive the daemon boundary",
          "[jmap][auth][onboarding]")
{
    const javelin::app::AccountAuthenticationResult result{
        .succeeded = true,
        .error = {},
        .sessionUrl = QStringLiteral("https://mail.example.com/.well-known/jmap"),
        .accessToken = QStringLiteral("access-token"),
        .refreshToken = QStringLiteral("refresh-token"),
        .tokenEndpoint = QStringLiteral("https://auth.example.com/token"),
        .clientId = QStringLiteral("client-id"),
        .issuer = QStringLiteral("https://auth.example.com"),
        .resourceUrl = QStringLiteral("https://mail.example.com/jmap"),
        .scope = QStringLiteral("mail offline_access"),
        .revocationEndpoint = QStringLiteral("https://auth.example.com/revoke"),
        .registrationClientUri = QStringLiteral("https://auth.example.com/register/client-id"),
        .registrationAccessToken = QStringLiteral("registration-token"),
        .expiresAtEpochSeconds = 123456789,
        .features = {},
    };

    const auto encoded = javelin::app::remote::encode(result);
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto decoded =
        javelin::app::remote::decodeValue<javelin::app::AccountAuthenticationResult>(
            std::get<QByteArray>(encoded));
    REQUIRE(std::holds_alternative<javelin::app::AccountAuthenticationResult>(decoded));
    const auto& restored = std::get<javelin::app::AccountAuthenticationResult>(decoded);
    CHECK(restored.succeeded == result.succeeded);
    CHECK(restored.sessionUrl == result.sessionUrl);
    CHECK(restored.accessToken == result.accessToken);
    CHECK(restored.refreshToken == result.refreshToken);
    CHECK(restored.tokenEndpoint == result.tokenEndpoint);
    CHECK(restored.clientId == result.clientId);
    CHECK(restored.issuer == result.issuer);
    CHECK(restored.resourceUrl == result.resourceUrl);
    CHECK(restored.scope == result.scope);
    CHECK(restored.revocationEndpoint == result.revocationEndpoint);
    CHECK(restored.registrationClientUri == result.registrationClientUri);
    CHECK(restored.registrationAccessToken == result.registrationAccessToken);
    CHECK(restored.expiresAtEpochSeconds == result.expiresAtEpochSeconds);
}
