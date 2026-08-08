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

        [[nodiscard]] std::vector<MethodInvocation>
        convertInvocations(std::vector<RawMethodInvocation> rawInvocations)
        {
            std::vector<MethodInvocation> invocations;
            invocations.reserve(rawInvocations.size());
            for (auto& rawInvocation : rawInvocations)
            {
                invocations.push_back(MethodInvocation{
                    .name = std::move(rawInvocation.name),
                    .arguments = encodeJson(rawInvocation.arguments),
                    .callId = std::move(rawInvocation.callId),
                });
            }
            return invocations;
        }

        [[nodiscard]] std::optional<std::vector<RawMethodInvocation>>
        convertInvocations(const std::vector<MethodInvocation>& invocations)
        {
            std::vector<RawMethodInvocation> rawInvocations;
            rawInvocations.reserve(invocations.size());
            for (const auto& invocation : invocations)
            {
                const auto decodedArguments = decodeJson(invocation.arguments);
                if (!decodedArguments.has_value())
                    return std::nullopt;

                rawInvocations.push_back(RawMethodInvocation{
                    .name = invocation.name,
                    .arguments = std::move(*decodedArguments),
                    .callId = invocation.callId,
                });
            }
            return rawInvocations;
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

        template <typename RawEnvelope>
        [[nodiscard]] std::optional<std::string> serializeRequest(const RequestEnvelope& request,
                                                                  RawEnvelope envelope)
        {
            auto methodCalls = convertInvocations(request.methodCalls);
            if (!methodCalls.has_value())
                return std::nullopt;

            envelope.usingCapabilities = request.usingCapabilities;
            envelope.methodCalls = std::move(*methodCalls);
            envelope.createdIds = request.createdIds;
            return serializeEnvelope(envelope);
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
            .methodCalls = convertInvocations(std::move(parsed.value->methodCalls)),
            .createdIds = std::move(parsed.value->createdIds),
        };

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
            .methodResponses = convertInvocations(std::move(parsed.value->methodResponses)),
            .createdIds = std::move(parsed.value->createdIds),
            .sessionState = std::move(parsed.value->sessionState),
        };

        return {
            .value = std::move(envelope),
            .error = std::nullopt,
        };
    }

    std::optional<std::string> serializeRequestEnvelope(const RequestEnvelope& request)
    {
        return serializeRequest(request, RawRequestEnvelope{});
    }

    std::optional<std::string> serializeWebSocketRequestEnvelope(const RequestEnvelope& request,
                                                                 const std::string_view requestId)
    {
        return serializeRequest(request, RawWebSocketRequestEnvelope{
                                             .type = "Request",
                                             .id = std::string{requestId},
                                             .usingCapabilities = {},
                                             .methodCalls = {},
                                             .createdIds = std::nullopt,
                                         });
    }

    std::optional<std::string> serializeResponseEnvelope(const ResponseEnvelope& response)
    {
        auto methodResponses = convertInvocations(response.methodResponses);
        if (!methodResponses.has_value())
            return std::nullopt;

        return serializeEnvelope(RawResponseEnvelope{
            .methodResponses = std::move(*methodResponses),
            .createdIds = response.createdIds,
            .sessionState = response.sessionState,
        });
    }

} // namespace javelin::jmap::api
