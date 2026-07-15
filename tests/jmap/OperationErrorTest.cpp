#include "jmap/OperationError.h"

#include "jmap/api/Error.h"
#include "jmap/api/ResponseReader.h"

#include <catch2/catch_test_macros.hpp>

#include <QNetworkReply>

TEST_CASE("transport failures are classified for application policy")
{
    using javelin::jmap::OperationErrorCode;
    using javelin::jmap::api::TransportError;
    using javelin::jmap::api::TransportErrorCode;

    const auto timeout = javelin::jmap::operationError(TransportError{
        .code = TransportErrorCode::NetworkFailure,
        .message = "timed out",
        .httpStatus = std::nullopt,
        .networkError = static_cast<int>(QNetworkReply::TimeoutError),
    });
    CHECK(timeout.code == OperationErrorCode::Timeout);
    CHECK(javelin::jmap::isTransientError(timeout));

    const auto authentication = javelin::jmap::operationError(TransportError{
        .code = TransportErrorCode::HttpFailure,
        .message = "unauthorized",
        .httpStatus = 401,
    });
    CHECK(authentication.code == OperationErrorCode::AuthenticationRequired);
    CHECK(javelin::jmap::isAuthenticationError(authentication));

    const auto rateLimit = javelin::jmap::operationError(TransportError{
        .code = TransportErrorCode::HttpFailure,
        .message = "slow down",
        .httpStatus = 429,
        .retryAfter = std::chrono::seconds{45},
    });
    CHECK(rateLimit.code == OperationErrorCode::RateLimited);
    REQUIRE(rateLimit.retryAfter.has_value());
    CHECK(*rateLimit.retryAfter == std::chrono::seconds{45});
}

TEST_CASE("JMAP method failures retain actionable distinctions")
{
    using javelin::jmap::OperationErrorCode;
    using javelin::jmap::api::MethodError;

    CHECK(javelin::jmap::operationError(MethodError{.type = "serverUnavailable"}).code ==
          OperationErrorCode::ServerUnavailable);
    CHECK(javelin::jmap::operationError(MethodError{.type = "forbidden"}).code ==
          OperationErrorCode::PermissionDenied);
    CHECK(javelin::jmap::operationError(MethodError{.type = "stateMismatch"}).code ==
          OperationErrorCode::Conflict);
    CHECK(javelin::jmap::operationError(MethodError{.type = "noSupportedScheduleMethods"}).code ==
          OperationErrorCode::SchedulingUnsupported);
    CHECK(javelin::jmap::operationError(MethodError{.type = "futureUnknownError"}).code ==
          OperationErrorCode::ServerFailure);
}

TEST_CASE("response reader preserves JMAP method error classification")
{
    const auto error = javelin::jmap::operationError(javelin::jmap::api::ResponseReaderError{
        .code = javelin::jmap::api::ResponseReaderErrorCode::MethodError,
        .message = "temporarily unavailable",
        .methodError = javelin::jmap::api::MethodError{.type = "serverUnavailable",
                                                       .description = "temporarily unavailable"},
    });

    CHECK(error.code == javelin::jmap::OperationErrorCode::ServerUnavailable);
    CHECK(javelin::jmap::isTransientError(error));
}
