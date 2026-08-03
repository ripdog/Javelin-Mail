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
    CHECK(restored.expiresAtEpochSeconds == result.expiresAtEpochSeconds);
}
