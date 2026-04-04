#include "jmap/auth/AccessTokenResolver.h"

#include <chrono>

namespace javelin::jmap::auth
{

    AccessTokenResolver::AccessTokenResolver(const TokenRefresher* tokenRefresher,
                                             SecretStore* secretStore)
        : m_tokenRefresher(tokenRefresher), m_secretStore(secretStore)
    {
    }

    AccessTokenResult AccessTokenResolver::resolve(const AccountCredentials& credentials) const
    {
        using namespace std::chrono_literals;

        OAuthToken token = credentials.token;
        if (!token.hasAccessToken())
        {
            return javelin::jmap::api::AuthError{
                .code = javelin::jmap::api::AuthErrorCode::MissingAccessToken,
                .message = "JMAP request requires an access token",
            };
        }

        if (!token.isExpired(Clock::now(), 60s))
        {
            return token;
        }

        if (!token.hasRefreshToken())
        {
            return javelin::jmap::api::AuthError{
                .code = javelin::jmap::api::AuthErrorCode::MissingRefreshToken,
                .message = "Access token is expired and no refresh token is available",
            };
        }

        if (m_tokenRefresher == nullptr)
        {
            return javelin::jmap::api::AuthError{
                .code = javelin::jmap::api::AuthErrorCode::TokenExpired,
                .message = "Access token is expired and no token refresher is configured",
            };
        }

        const auto refreshResult = m_tokenRefresher->refresh(credentials);
        if (std::holds_alternative<javelin::jmap::api::AuthError>(refreshResult))
        {
            return std::get<javelin::jmap::api::AuthError>(refreshResult);
        }

        token = std::get<OAuthToken>(refreshResult);

        if (m_secretStore != nullptr && !m_secretStore->store(credentials.accountId, token))
        {
            return javelin::jmap::api::AuthError{
                .code = javelin::jmap::api::AuthErrorCode::SecretStoreFailure,
                .message = "Failed to persist refreshed token",
            };
        }

        return token;
    }

} // namespace javelin::jmap::auth
