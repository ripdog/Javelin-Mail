#include "jmap/OperationError.h"

#include "jmap/api/Error.h"
#include "jmap/api/ResponseReader.h"
#include "storage/DatabaseError.h"

#include <QNetworkReply>

namespace javelin::jmap
{

    bool isCancellation(const OperationError& error)
    {
        return error.code == OperationErrorCode::Cancelled;
    }

    bool isAuthenticationError(const OperationError& error)
    {
        return error.code == OperationErrorCode::AuthenticationRequired;
    }

    bool isTransientError(const OperationError& error)
    {
        switch (error.code)
        {
        case OperationErrorCode::NetworkUnavailable:
        case OperationErrorCode::Timeout:
        case OperationErrorCode::RateLimited:
        case OperationErrorCode::ServerUnavailable:
        case OperationErrorCode::LocalStorageBusy:
            return true;
        default:
            return false;
        }
    }

    bool requiresUserIntervention(const OperationError& error)
    {
        return !isCancellation(error) && !isTransientError(error);
    }

    std::string_view toString(const OperationErrorCode code)
    {
        switch (code)
        {
        case OperationErrorCode::Cancelled:
            return "cancelled";
        case OperationErrorCode::NetworkUnavailable:
            return "network_unavailable";
        case OperationErrorCode::Timeout:
            return "timeout";
        case OperationErrorCode::HttpFailure:
            return "http_failure";
        case OperationErrorCode::AuthenticationRequired:
            return "authentication_required";
        case OperationErrorCode::PermissionDenied:
            return "permission_denied";
        case OperationErrorCode::RateLimited:
            return "rate_limited";
        case OperationErrorCode::ServerUnavailable:
            return "server_unavailable";
        case OperationErrorCode::ServerFailure:
            return "server_failure";
        case OperationErrorCode::ProtocolViolation:
            return "protocol_violation";
        case OperationErrorCode::UnsupportedCapability:
            return "unsupported_capability";
        case OperationErrorCode::LocalStorageBusy:
            return "local_storage_busy";
        case OperationErrorCode::LocalStorageFailure:
            return "local_storage_failure";
        case OperationErrorCode::InvalidRequest:
            return "invalid_request";
        case OperationErrorCode::InvalidUserInput:
            return "invalid_user_input";
        case OperationErrorCode::PreconditionFailed:
            return "precondition_failed";
        case OperationErrorCode::Conflict:
            return "conflict";
        case OperationErrorCode::NotFound:
            return "not_found";
        case OperationErrorCode::SchedulingUnsupported:
            return "scheduling_unsupported";
        }
        return "unknown_operation_error";
    }

    OperationError operationError(const api::TransportError& error)
    {
        auto code = OperationErrorCode::NetworkUnavailable;
        if (error.code == api::TransportErrorCode::Cancelled)
            code = OperationErrorCode::Cancelled;
        else if (error.networkError == QNetworkReply::TimeoutError)
            code = OperationErrorCode::Timeout;
        else if (error.httpStatus == 401)
            code = OperationErrorCode::AuthenticationRequired;
        else if (error.httpStatus == 403)
            code = OperationErrorCode::PermissionDenied;
        else if (error.httpStatus == 408)
            code = OperationErrorCode::Timeout;
        else if (error.httpStatus == 429)
            code = OperationErrorCode::RateLimited;
        else if (error.httpStatus == 502 || error.httpStatus == 503 || error.httpStatus == 504)
            code = OperationErrorCode::ServerUnavailable;
        else if (error.httpStatus.has_value() && *error.httpStatus >= 500 &&
                 *error.httpStatus <= 599)
            code = OperationErrorCode::ServerFailure;
        else if (error.code == api::TransportErrorCode::HttpFailure)
            code = OperationErrorCode::HttpFailure;
        else if (error.code == api::TransportErrorCode::ResponseDecodingFailed)
            code = OperationErrorCode::ProtocolViolation;

        return {
            .code = code,
            .message = QString::fromStdString(error.message),
            .httpStatus = error.httpStatus,
            .retryAfter = error.retryAfter,
        };
    }

    OperationError operationError(const OperationError& error)
    {
        return error;
    }

    OperationError operationError(const api::AuthError& error)
    {
        return {
            .code = OperationErrorCode::AuthenticationRequired,
            .message = QString::fromStdString(error.message),
        };
    }

    OperationError operationError(const api::ProtocolError& error)
    {
        return {
            .code = error.code == api::ProtocolErrorCode::InvalidRequest
                        ? OperationErrorCode::InvalidRequest
                    : error.code == api::ProtocolErrorCode::UnsupportedFeature ||
                            error.code == api::ProtocolErrorCode::CapabilityNegotiationFailed
                        ? OperationErrorCode::UnsupportedCapability
                        : OperationErrorCode::ProtocolViolation,
            .message = QString::fromStdString(error.message),
        };
    }

    OperationError operationError(const api::MethodError& error)
    {
        auto code = OperationErrorCode::ServerFailure;
        if (error.type == "serverUnavailable")
            code = OperationErrorCode::ServerUnavailable;
        else if (error.type == "forbidden")
            code = OperationErrorCode::PermissionDenied;
        else if (error.type == "accountNotFound")
            code = OperationErrorCode::NotFound;
        else if (error.type == "accountNotSupportedByMethod" || error.type == "unknownMethod")
            code = OperationErrorCode::UnsupportedCapability;
        else if (error.type == "accountReadOnly")
            code = OperationErrorCode::PermissionDenied;
        else if (error.type == "invalidArguments" || error.type == "invalidResultReference")
            code = OperationErrorCode::InvalidRequest;
        else if (error.type == "stateMismatch")
            code = OperationErrorCode::Conflict;
        else if (error.type == "rateLimit")
            code = OperationErrorCode::RateLimited;
        else if (error.type == "noSupportedScheduleMethods")
            code = OperationErrorCode::SchedulingUnsupported;

        return {
            .code = code,
            .message = error.description.has_value() ? QString::fromStdString(*error.description)
                                                     : QStringLiteral("JMAP method failed: %1")
                                                           .arg(QString::fromStdString(error.type)),
            .protocolType = error.type,
        };
    }

    OperationError operationError(const api::ResponseReaderError& error)
    {
        if (error.methodError.has_value())
            return operationError(*error.methodError);
        return {
            .code = OperationErrorCode::ProtocolViolation,
            .message = QString::fromStdString(error.message),
        };
    }

    OperationError operationError(const cache::DatabaseError& error)
    {
        return {
            .code = error.code == cache::DatabaseErrorCode::TransientContention
                        ? OperationErrorCode::LocalStorageBusy
                        : OperationErrorCode::LocalStorageFailure,
            .message = error.message,
        };
    }

} // namespace javelin::jmap
