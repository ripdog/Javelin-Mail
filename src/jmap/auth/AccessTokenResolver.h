#pragma once

#include "jmap/api/Error.h"
#include "jmap/auth/Auth.h"

#include <variant>

namespace javelin::jmap::auth
{

    using AccessTokenResult = std::variant<OAuthToken, javelin::jmap::api::AuthError>;

    class AccessTokenResolver
    {
      public:
        AccessTokenResolver(const TokenRefresher* tokenRefresher = nullptr,
                            SecretStore* secretStore = nullptr);

        [[nodiscard]] AccessTokenResult resolve(const AccountCredentials& credentials) const;

      private:
        const TokenRefresher* m_tokenRefresher;
        SecretStore* m_secretStore;
    };

} // namespace javelin::jmap::auth
