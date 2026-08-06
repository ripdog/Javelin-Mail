#pragma once

#include <cstdint>
#include <string>

namespace javelin::app
{

    struct AccountConnectionSettings
    {
        std::string connectionId;
        std::uint64_t revision = 0;
        std::string displayName = {};
        std::string sessionUrl;
        std::string loginEmail;
        std::string apiKey;
        std::string refreshToken;
        std::string tokenEndpoint;
        std::string oauthClientId;
        std::string oauthIssuer = {};
        std::string oauthResource = {};
        std::string oauthScope = {};
        std::string revocationEndpoint = {};
        std::string registrationClientUri = {};
        std::string registrationAccessToken = {};

        friend bool operator==(const AccountConnectionSettings&,
                               const AccountConnectionSettings&) = default;
    };

} // namespace javelin::app
