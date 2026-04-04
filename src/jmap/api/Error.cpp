#include "jmap/api/Error.h"

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
        case ProtocolErrorCode::InvalidResponse:
            return "invalid_response";
        case ProtocolErrorCode::CapabilityNegotiationFailed:
            return "capability_negotiation_failed";
        case ProtocolErrorCode::UnsupportedFeature:
            return "unsupported_feature";
        }

        return "unknown_protocol_error";
    }

} // namespace javelin::jmap::api
