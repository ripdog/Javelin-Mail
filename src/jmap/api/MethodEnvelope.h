#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::api
{

    struct MethodInvocation
    {
        std::string name;
        std::string arguments;
        std::string callId;
    };

    struct RequestEnvelope
    {
        std::vector<std::string> usingCapabilities;
        std::vector<MethodInvocation> methodCalls;
        std::optional<std::unordered_map<std::string, std::string>> createdIds;
    };

    struct ResponseEnvelope
    {
        std::vector<MethodInvocation> methodResponses;
        std::optional<std::unordered_map<std::string, std::string>> createdIds;
        std::string sessionState;
    };

    template <typename T> struct ParsedEnvelope
    {
        std::optional<T> value;
        std::optional<std::string> error;

        [[nodiscard]] bool ok() const
        {
            return value.has_value();
        }
    };

    [[nodiscard]] ParsedEnvelope<RequestEnvelope> parseRequestEnvelope(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ResponseEnvelope> parseResponseEnvelope(std::string_view json);

    [[nodiscard]] std::optional<std::string>
    serializeRequestEnvelope(const RequestEnvelope& request);
    [[nodiscard]] std::optional<std::string>
    serializeWebSocketRequestEnvelope(const RequestEnvelope& request, std::string_view requestId);
    [[nodiscard]] std::optional<std::string>
    serializeResponseEnvelope(const ResponseEnvelope& response);

} // namespace javelin::jmap::api
