#include "jmap/api/Error.h"
#include "jmap/api/MethodEnvelope.h"

#include <glaze/glaze.hpp>

namespace
{

    struct RawMethodError
    {
        std::string type;
        std::optional<std::string> description;
    };

} // namespace

template <> struct glz::meta<RawMethodError>
{
    using T = RawMethodError;

    static constexpr auto value = glz::object("type", &T::type, "description", &T::description);
};

namespace javelin::jmap::api
{

    std::string_view toString(const TransportErrorCode code)
    {
        switch (code)
        {
        case TransportErrorCode::Cancelled:
            return "cancelled";
        case TransportErrorCode::NetworkFailure:
            return "network_failure";
        case TransportErrorCode::HttpFailure:
            return "http_failure";
        case TransportErrorCode::LocalIoFailure:
            return "local_io_failure";
        case TransportErrorCode::ResponseDecodingFailed:
            return "response_decoding_failed";
        }

        return "unknown_transport_error";
    }

    std::string_view toString(const AuthErrorCode code)
    {
        switch (code)
        {
        case AuthErrorCode::MissingAccessToken:
            return "missing_access_token";
        case AuthErrorCode::MissingRefreshToken:
            return "missing_refresh_token";
        case AuthErrorCode::TokenExpired:
            return "token_expired";
        case AuthErrorCode::TokenRefreshFailed:
            return "token_refresh_failed";
        case AuthErrorCode::SecretStoreFailure:
            return "secret_store_failure";
        }

        return "unknown_auth_error";
    }

    std::string_view toString(const ProtocolErrorCode code)
    {
        switch (code)
        {
        case ProtocolErrorCode::InvalidRequest:
            return "invalid_request";
        case ProtocolErrorCode::InvalidResponse:
            return "invalid_response";
        case ProtocolErrorCode::CapabilityNegotiationFailed:
            return "capability_negotiation_failed";
        case ProtocolErrorCode::UnsupportedFeature:
            return "unsupported_feature";
        }

        return "unknown_protocol_error";
    }

    ParsedEnvelope<MethodError> parseMethodError(const std::string_view json)
    {
        std::string buffer{json};
        RawMethodError raw{};
        const auto readError = glz::read<glz::opts{.error_on_unknown_keys = false}>(raw, buffer);
        if (readError)
        {
            return {
                .value = std::nullopt,
                .error = glz::format_error(readError, buffer),
            };
        }

        return {
            .value =
                MethodError{
                    .type = std::move(raw.type),
                    .description = std::move(raw.description),
                },
            .error = std::nullopt,
        };
    }

} // namespace javelin::jmap::api
