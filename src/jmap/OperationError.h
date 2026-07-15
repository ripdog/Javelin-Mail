#pragma once

#include <QMetaType>
#include <QString>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace javelin::jmap::api
{
    struct AuthError;
    struct MethodError;
    struct ProtocolError;
    struct ResponseReaderError;
    struct TransportError;
} // namespace javelin::jmap::api

namespace javelin::jmap::cache
{
    struct DatabaseError;
}

namespace javelin::jmap
{

    enum class OperationErrorCode
    {
        Cancelled,
        NetworkUnavailable,
        Timeout,
        HttpFailure,
        AuthenticationRequired,
        PermissionDenied,
        RateLimited,
        ServerUnavailable,
        ServerFailure,
        ProtocolViolation,
        UnsupportedCapability,
        LocalStorageFailure,
        InvalidRequest,
        InvalidUserInput,
        PreconditionFailed,
        Conflict,
        NotFound,
        SchedulingUnsupported,
    };

    struct OperationError
    {
        OperationErrorCode code = OperationErrorCode::ServerFailure;
        QString message;
        std::optional<int> httpStatus = std::nullopt;
        std::optional<std::chrono::seconds> retryAfter = std::nullopt;
        std::optional<std::string> protocolType = std::nullopt;
    };

    [[nodiscard]] bool isCancellation(const OperationError& error);
    [[nodiscard]] bool isAuthenticationError(const OperationError& error);
    [[nodiscard]] bool isTransientError(const OperationError& error);
    [[nodiscard]] bool requiresUserIntervention(const OperationError& error);
    [[nodiscard]] std::string_view toString(OperationErrorCode code);

    [[nodiscard]] OperationError operationError(const api::TransportError& error);
    [[nodiscard]] OperationError operationError(const OperationError& error);
    [[nodiscard]] OperationError operationError(const api::AuthError& error);
    [[nodiscard]] OperationError operationError(const api::ProtocolError& error);
    [[nodiscard]] OperationError operationError(const api::MethodError& error);
    [[nodiscard]] OperationError operationError(const api::ResponseReaderError& error);
    [[nodiscard]] OperationError operationError(const cache::DatabaseError& error);

} // namespace javelin::jmap

Q_DECLARE_METATYPE(javelin::jmap::OperationError)
