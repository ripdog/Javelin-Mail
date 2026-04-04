#include "jmap/auth/Auth.h"
#include "jmap/api/Error.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

TEST_CASE("oauth token expiry respects skew handling", "[jmap][auth]")
{
    using namespace std::chrono_literals;

    const auto now = javelin::jmap::auth::Clock::now();
    const javelin::jmap::auth::OAuthToken token{
        .accessToken = "token",
        .refreshToken = "refresh",
        .expiry = now + 30s,
    };

    CHECK(token.hasAccessToken());
    CHECK(token.hasRefreshToken());
    CHECK(token.isExpired(now));
    CHECK_FALSE(token.isExpired(now, 5s));
}

TEST_CASE("oauth token without expiry is treated as active", "[jmap][auth]")
{
    const javelin::jmap::auth::OAuthToken token{
        .accessToken = "token",
        .refreshToken = std::nullopt,
        .expiry = std::nullopt,
    };

    CHECK(token.hasAccessToken());
    CHECK_FALSE(token.hasRefreshToken());
    CHECK_FALSE(token.isExpired(javelin::jmap::auth::Clock::now()));
}

TEST_CASE("typed auth and transport error codes have stable string mappings", "[jmap][auth]")
{
    CHECK(javelin::jmap::api::toString(javelin::jmap::api::AuthErrorCode::TokenRefreshFailed) ==
          std::string_view("token_refresh_failed"));
    CHECK(javelin::jmap::api::toString(javelin::jmap::api::TransportErrorCode::HttpFailure) ==
          std::string_view("http_failure"));
    CHECK(javelin::jmap::api::toString(
              javelin::jmap::api::ProtocolErrorCode::CapabilityNegotiationFailed) ==
          std::string_view("capability_negotiation_failed"));
}
