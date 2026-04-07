#include "jmap/api/SessionClient.h"
#include "FixtureReader.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace
{

    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }

        static int argc = 1;
        static char appName[] = "javelin-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        javelin::jmap::api::HttpRequest lastRequest;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(const javelin::jmap::api::HttpRequest& request) override
        {
            lastRequest = request;
            REQUIRE_FALSE(queuedResults.empty());

            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }
    };

    class FakeTokenRefresher final : public javelin::jmap::auth::TokenRefresher
    {
      public:
        mutable std::size_t calls = 0;
        javelin::jmap::auth::TokenRefreshResult result;

        [[nodiscard]] javelin::jmap::auth::TokenRefreshResult
        refresh(const javelin::jmap::auth::AccountCredentials&) const override
        {
            ++calls;
            return result;
        }
    };

    class FakeSecretStore final : public javelin::jmap::auth::SecretStore
    {
      public:
        mutable std::optional<javelin::jmap::auth::OAuthToken> storedToken;
        bool storeResult = true;

        [[nodiscard]] std::optional<javelin::jmap::auth::OAuthToken>
        load(std::string_view) const override
        {
            return storedToken;
        }

        bool store(std::string_view, const javelin::jmap::auth::OAuthToken& token) override
        {
            storedToken = token;
            return storeResult;
        }

        bool clear(std::string_view) override
        {
            storedToken.reset();
            return true;
        }
    };

    [[nodiscard]] javelin::jmap::auth::SessionRequestContext makeRequestContext()
    {
        return javelin::jmap::auth::SessionRequestContext{
            .credentials =
                {
                    .accountId = "u1",
                    .emailAddress = "alice@example.com",
                    .sessionUrl = "https://mail.example.com/.well-known/jmap",
                    .token =
                        {
                            .accessToken = "access-token",
                            .refreshToken = "refresh-token",
                            .expiry = std::nullopt,
                        },
                },
            .requiredCapabilities =
                {
                    .mail = true,
                    .submission = true,
                },
        };
    }

} // namespace

TEST_CASE("session client discovers a session through the transport abstraction",
          "[jmap][transport]")
{
    ensureApplication();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/basic_session.json")),
    });

    javelin::jmap::api::SessionClient sessionClient{transport};
    const auto result = QCoro::waitFor(sessionClient.discover(makeRequestContext()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::Session>(result));
    const auto& session = std::get<javelin::jmap::api::Session>(result);
    CHECK(session.username == "alice@example.com");
    CHECK(transport.lastRequest.url ==
          QUrl{QStringLiteral("https://mail.example.com/.well-known/jmap")});
    REQUIRE(transport.lastRequest.headers.size() == 2);
    CHECK(transport.lastRequest.headers.front().name == "Authorization");
    CHECK(transport.lastRequest.headers.front().value == "Bearer access-token");
}

TEST_CASE("session client refreshes expired tokens and persists them when configured",
          "[jmap][transport][auth]")
{
    ensureApplication();

    using namespace std::chrono_literals;

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/basic_session.json")),
    });

    FakeTokenRefresher tokenRefresher;
    tokenRefresher.result = javelin::jmap::auth::OAuthToken{
        .accessToken = "refreshed-token",
        .refreshToken = "refresh-token",
        .expiry = javelin::jmap::auth::Clock::now() + 10min,
    };
    FakeSecretStore secretStore;

    auto requestContext = makeRequestContext();
    requestContext.credentials.token.expiry = javelin::jmap::auth::Clock::now();

    javelin::jmap::api::SessionClient sessionClient{transport, &tokenRefresher, &secretStore};
    const auto result = QCoro::waitFor(sessionClient.discover(requestContext));

    REQUIRE(std::holds_alternative<javelin::jmap::api::Session>(result));
    CHECK(tokenRefresher.calls == 1);
    REQUIRE(secretStore.storedToken.has_value());
    CHECK(secretStore.storedToken->accessToken == "refreshed-token");
    CHECK(transport.lastRequest.headers.front().value == "Bearer refreshed-token");
}

TEST_CASE("session client returns an auth error when the token is expired without refresh support",
          "[jmap][transport][auth]")
{
    ensureApplication();

    auto requestContext = makeRequestContext();
    requestContext.credentials.token.expiry = javelin::jmap::auth::Clock::now();
    requestContext.credentials.token.refreshToken.reset();

    FakeTransport transport;
    javelin::jmap::api::SessionClient sessionClient{transport};
    const auto result = QCoro::waitFor(sessionClient.discover(requestContext));

    REQUIRE(std::holds_alternative<javelin::jmap::api::AuthError>(result));
    CHECK(std::get<javelin::jmap::api::AuthError>(result).code ==
          javelin::jmap::api::AuthErrorCode::MissingRefreshToken);
}

TEST_CASE("session client propagates transport failures", "[jmap][transport]")
{
    ensureApplication();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
        .message = "Unauthorized",
        .httpStatus = 401,
    });

    javelin::jmap::api::SessionClient sessionClient{transport};
    const auto result = QCoro::waitFor(sessionClient.discover(makeRequestContext()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::TransportError>(result));
    CHECK(std::get<javelin::jmap::api::TransportError>(result).httpStatus == 401);
}

TEST_CASE("session client maps capability validation failures to protocol errors",
          "[jmap][transport]")
{
    ensureApplication();

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/missing_mail_capability.json")),
    });

    javelin::jmap::api::SessionClient sessionClient{transport};
    const auto result = QCoro::waitFor(sessionClient.discover(makeRequestContext()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ProtocolError>(result));
    CHECK(std::get<javelin::jmap::api::ProtocolError>(result).code ==
          javelin::jmap::api::ProtocolErrorCode::CapabilityNegotiationFailed);
}
