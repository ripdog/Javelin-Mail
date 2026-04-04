#pragma once

#include "jmap/api/Error.h"
#include "jmap/api/Session.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace javelin::jmap::auth
{

    using Clock = std::chrono::system_clock;

    struct OAuthToken
    {
        std::string accessToken;
        std::optional<std::string> refreshToken;
        std::optional<Clock::time_point> expiry;

        [[nodiscard]] bool hasAccessToken() const;
        [[nodiscard]] bool hasRefreshToken() const;
        [[nodiscard]] bool isExpired(Clock::time_point now,
                                     std::chrono::seconds skew = std::chrono::minutes{1}) const;
    };

    struct AccountCredentials
    {
        std::string accountId;
        std::string emailAddress;
        std::string sessionUrl;
        OAuthToken token;
    };

    struct SessionRequestContext
    {
        AccountCredentials credentials;
        javelin::jmap::api::RequiredCapabilities requiredCapabilities;
    };

    using TokenRefreshResult = std::variant<OAuthToken, javelin::jmap::api::AuthError>;

    class SecretStore
    {
      public:
        virtual ~SecretStore() = default;

        [[nodiscard]] virtual std::optional<OAuthToken> load(std::string_view accountId) const = 0;
        virtual bool store(std::string_view accountId, const OAuthToken& token) = 0;
        virtual bool clear(std::string_view accountId) = 0;
    };

    class TokenRefresher
    {
      public:
        virtual ~TokenRefresher() = default;

        [[nodiscard]] virtual TokenRefreshResult
        refresh(const AccountCredentials& credentials) const = 0;
    };

} // namespace javelin::jmap::auth
