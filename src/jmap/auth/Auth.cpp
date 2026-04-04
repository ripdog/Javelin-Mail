#include "jmap/auth/Auth.h"

namespace javelin::jmap::auth
{

    bool OAuthToken::hasAccessToken() const
    {
        return !accessToken.empty();
    }

    bool OAuthToken::hasRefreshToken() const
    {
        return refreshToken.has_value() && !refreshToken->empty();
    }

    bool OAuthToken::isExpired(const Clock::time_point now, const std::chrono::seconds skew) const
    {
        if (!expiry.has_value())
        {
            return false;
        }

        return *expiry <= now + skew;
    }

} // namespace javelin::jmap::auth
