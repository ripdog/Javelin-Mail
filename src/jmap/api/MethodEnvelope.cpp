#include "jmap/api/MethodEnvelope.h"

#include <glaze/glaze.hpp>

#include <string>

namespace
{

    struct RawMethodInvocation
    {
        std::string name;
        glz::generic arguments;
        std::string callId;
    };

    struct RawRequestEnvelope
    {
        std::vector<std::string> usingCapabilities;
        std::vector<RawMethodInvocation> methodCalls;
        std::optional<std::unordered_map<std::string, std::string>> createdIds;
    };

    struct RawWebSocketRequestEnvelope
    {
        std::string type = "Request";
        std::string id;
        std::vector<std::string> usingCapabilities;
        std::vector<RawMethodInvocation> methodCalls;
        std::optional<std::unordered_map<std::string, std::string>> createdIds;
    };

    struct RawResponseEnvelope
    {
        std::vector<RawMethodInvocation> methodResponses;
        std::optional<std::unordered_map<std::string, std::string>> createdIds;
        std::string sessionState;
    };

} // namespace

template <> struct glz::meta<RawMethodInvocation>
{
    using T = RawMethodInvocation;

    static constexpr auto value = glz::array(&T::name, &T::arguments, &T::callId);
};

template <> struct glz::meta<RawRequestEnvelope>
{
    using T = RawRequestEnvelope;

    static constexpr auto value = glz::object("using", &T::usingCapabilities, "methodCalls",
                                              &T::methodCalls, "createdIds", &T::createdIds);
};

template <> struct glz::meta<RawWebSocketRequestEnvelope>
{
    using T = RawWebSocketRequestEnvelope;

    static constexpr auto value =
        glz::object("@type", &T::type, "id", &T::id, "using", &T::usingCapabilities, "methodCalls",
                    &T::methodCalls, "createdIds", &T::createdIds);
};

template <> struct glz::meta<RawResponseEnvelope>
{
    using T = RawResponseEnvelope;

    static constexpr auto value = glz::object("methodResponses", &T::methodResponses, "createdIds",
                                              &T::createdIds, "sessionState", &T::sessionState);
};

namespace javelin::jmap::api
{

    namespace
    {

        [[nodiscard]] std::string encodeJson(const glz::generic& value)
        {
            std::string buffer;
            const auto writeError = glz::write_json(value, buffer);
            if (writeError)
            {
                return {};
            }

            return buffer;
        }

        [[nodiscard]] std::optional<glz::generic> decodeJson(const std::string& json)
        {
            std::string buffer{json};
            glz::generic value;
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(value, buffer);
            if (readError)
            {
                return std::nullopt;
            }

            return value;
        }

        [[nodiscard]] MethodInvocation convertInvocation(RawMethodInvocation rawInvocation)
        {
            return MethodInvocation{
                .name = std::move(rawInvocation.name),
                .arguments = encodeJson(rawInvocation.arguments),
                .callId = std::move(rawInvocation.callId),
            };
        }

        template <typename T> [[nodiscard]] ParsedEnvelope<T> parseEnvelope(std::string_view json)
        {
            std::string buffer{json};
            T envelope{};
            const auto readError =
                glz::read<glz::opts{.error_on_unknown_keys = false}>(envelope, buffer);
            if (readError)
            {
                return {
                    .value = std::nullopt,
                    .error = glz::format_error(readError, buffer),
                };
            }

            return {
                .value = std::move(envelope),
                .error = std::nullopt,
            };
        }

        template <typename T>
        [[nodiscard]] std::optional<std::string> serializeEnvelope(const T& envelope)
        {
            std::string buffer;
            const auto writeError = glz::write_json(envelope, buffer);
            if (writeError)
            {
                return std::nullopt;
            }

            return buffer;
        }

    } // namespace

    ParsedEnvelope<RequestEnvelope> parseRequestEnvelope(std::string_view json)
    {
        const auto parsed = parseEnvelope<RawRequestEnvelope>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        RequestEnvelope envelope{
            .usingCapabilities = std::move(parsed.value->usingCapabilities),
            .methodCalls = {},
            .createdIds = std::move(parsed.value->createdIds),
        };
        envelope.methodCalls.reserve(parsed.value->methodCalls.size());
        for (auto& methodCall : parsed.value->methodCalls)
        {
            envelope.methodCalls.push_back(convertInvocation(methodCall));
        }

        return {
            .value = std::move(envelope),
            .error = std::nullopt,
        };
    }

    ParsedEnvelope<ResponseEnvelope> parseResponseEnvelope(std::string_view json)
    {
        const auto parsed = parseEnvelope<RawResponseEnvelope>(json);
        if (!parsed.ok())
        {
            return {
                .value = std::nullopt,
                .error = parsed.error,
            };
        }

        ResponseEnvelope envelope{
            .methodResponses = {},
            .createdIds = std::move(parsed.value->createdIds),
            .sessionState = std::move(parsed.value->sessionState),
        };
        envelope.methodResponses.reserve(parsed.value->methodResponses.size());
        for (auto& methodResponse : parsed.value->methodResponses)
        {
            envelope.methodResponses.push_back(convertInvocation(methodResponse));
        }

        return {
            .value = std::move(envelope),
            .error = std::nullopt,
        };
    }

    std::optional<std::string> serializeRequestEnvelope(const RequestEnvelope& request)
    {
        RawRequestEnvelope envelope{
            .usingCapabilities = request.usingCapabilities,
            .methodCalls = {},
            .createdIds = request.createdIds,
        };
        envelope.methodCalls.reserve(request.methodCalls.size());
        for (const auto& methodCall : request.methodCalls)
        {
            const auto decodedArguments = decodeJson(methodCall.arguments);
            if (!decodedArguments.has_value())
            {
                return std::nullopt;
            }

            envelope.methodCalls.push_back(RawMethodInvocation{
                .name = methodCall.name,
                .arguments = std::move(*decodedArguments),
                .callId = methodCall.callId,
            });
        }

        return serializeEnvelope(envelope);
    }

    std::optional<std::string> serializeWebSocketRequestEnvelope(const RequestEnvelope& request,
                                                                 const std::string_view requestId)
    {
        RawWebSocketRequestEnvelope envelope{
            .type = "Request",
            .id = std::string{requestId},
            .usingCapabilities = request.usingCapabilities,
            .methodCalls = {},
            .createdIds = request.createdIds,
        };
        envelope.methodCalls.reserve(request.methodCalls.size());
        for (const auto& methodCall : request.methodCalls)
        {
            const auto decodedArguments = decodeJson(methodCall.arguments);
            if (!decodedArguments.has_value())
            {
                return std::nullopt;
            }

            envelope.methodCalls.push_back(RawMethodInvocation{
                .name = methodCall.name,
                .arguments = std::move(*decodedArguments),
                .callId = methodCall.callId,
            });
        }

        return serializeEnvelope(envelope);
    }

    std::optional<std::string> serializeResponseEnvelope(const ResponseEnvelope& response)
    {
        RawResponseEnvelope envelope{
            .methodResponses = {},
            .createdIds = response.createdIds,
            .sessionState = response.sessionState,
        };
        envelope.methodResponses.reserve(response.methodResponses.size());
        for (const auto& methodResponse : response.methodResponses)
        {
            const auto decodedArguments = decodeJson(methodResponse.arguments);
            if (!decodedArguments.has_value())
            {
                return std::nullopt;
            }

            envelope.methodResponses.push_back(RawMethodInvocation{
                .name = methodResponse.name,
                .arguments = std::move(*decodedArguments),
                .callId = methodResponse.callId,
            });
        }

        return serializeEnvelope(envelope);
    }

} // namespace javelin::jmap::api
