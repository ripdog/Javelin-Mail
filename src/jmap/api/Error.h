#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace javelin::jmap::api
{

    template <typename T> struct ParsedEnvelope;

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
        std::optional<int> httpStatus = std::nullopt;
        std::optional<int> networkError = std::nullopt;
        std::optional<std::chrono::seconds> retryAfter = std::nullopt;
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
        InvalidRequest,
        InvalidResponse,
        CapabilityNegotiationFailed,
        UnsupportedFeature,
    };

    struct ProtocolError
    {
        ProtocolErrorCode code;
        std::string message;
    };

    struct MethodError
    {
        std::string type;
        std::optional<std::string> description = std::nullopt;
    };

    [[nodiscard]] std::string_view toString(TransportErrorCode code);
    [[nodiscard]] std::string_view toString(AuthErrorCode code);
    [[nodiscard]] std::string_view toString(ProtocolErrorCode code);
    [[nodiscard]] ParsedEnvelope<MethodError> parseMethodError(std::string_view json);

} // namespace javelin::jmap::api
