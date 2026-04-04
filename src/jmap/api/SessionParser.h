#pragma once

#include "jmap/api/Session.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::jmap::api
{

    enum class SessionParseErrorCode
    {
        JsonParseFailed,
        CapabilityValidationFailed,
    };

    struct SessionParseError
    {
        SessionParseErrorCode code;
        std::string message;
        std::vector<CapabilityError> capabilityErrors;
    };

    struct SessionParseResult
    {
        std::optional<Session> session;
        std::optional<SessionParseError> error;

        [[nodiscard]] bool ok() const;
    };

    [[nodiscard]] SessionParseResult parseSession(std::string_view json,
                                                  const RequiredCapabilities& required = {});

    [[nodiscard]] std::string_view toString(SessionParseErrorCode code);

} // namespace javelin::jmap::api
