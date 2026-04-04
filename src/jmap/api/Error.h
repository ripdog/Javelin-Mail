#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace javelin::jmap::api
{

    enum class TransportErrorCode
    {
        Cancelled,
        NetworkFailure,
        HttpFailure,
        ResponseDecodingFailed,
    };

    struct TransportError
    {
        TransportErrorCode code;
        std::string message;
        std::optional<int> httpStatus;
    };

    enum class AuthErrorCode
    {
        MissingAccessToken,
        MissingRefreshToken,
        TokenExpired,
        TokenRefreshFailed,
        SecretStoreFailure,
    };

    struct AuthError
    {
        AuthErrorCode code;
        std::string message;
    };

    enum class ProtocolErrorCode
    {
        InvalidResponse,
        CapabilityNegotiationFailed,
        UnsupportedFeature,
    };

    struct ProtocolError
    {
        ProtocolErrorCode code;
        std::string message;
    };

    [[nodiscard]] std::string_view toString(TransportErrorCode code);
    [[nodiscard]] std::string_view toString(AuthErrorCode code);
    [[nodiscard]] std::string_view toString(ProtocolErrorCode code);

} // namespace javelin::jmap::api
