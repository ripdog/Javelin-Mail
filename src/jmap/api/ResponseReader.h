#pragma once

#include "jmap/api/Error.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/RequestBuilder.h"

#include <optional>
#include <string>
#include <variant>

namespace javelin::jmap::api
{

    enum class ResponseReaderErrorCode
    {
        MissingMethodResponse,
        MethodError,
        UnexpectedMethodName,
        ParseFailed,
    };

    struct ResponseReaderError
    {
        ResponseReaderErrorCode code = ResponseReaderErrorCode::MissingMethodResponse;
        std::string message;
        std::optional<MethodError> methodError;
    };

    template <typename Response>
    using ResponseReadResult = std::variant<Response, ResponseReaderError>;

    class ResponseReader
    {
      public:
        explicit ResponseReader(const ResponseEnvelope& envelope) : m_envelope(envelope)
        {
        }

        [[nodiscard]] std::vector<MethodInvocation> rawAll(std::string_view callId) const
        {
            std::vector<MethodInvocation> methods;
            for (const auto& methodResponse : m_envelope.methodResponses)
            {
                if (methodResponse.callId == callId)
                {
                    methods.push_back(methodResponse);
                }
            }

            return methods;
        }

        [[nodiscard]] std::optional<MethodInvocation> raw(std::string_view callId) const
        {
            for (const auto& methodResponse : m_envelope.methodResponses)
            {
                if (methodResponse.callId == callId)
                {
                    return methodResponse;
                }
            }

            return std::nullopt;
        }

        template <typename Response>
        [[nodiscard]] ResponseReadResult<Response> get(const CallHandle<Response>& handle) const
        {
            const auto methods = rawAll(handle.callId);
            if (methods.empty())
            {
                return ResponseReaderError{
                    .code = ResponseReaderErrorCode::MissingMethodResponse,
                    .message = "The JMAP response did not contain the expected method response",
                    .methodError = std::nullopt,
                };
            }

            const auto expectedName = std::string{MethodResponseTraits<Response>::methodName};
            for (const auto& method : methods)
            {
                if (method.name != expectedName)
                {
                    continue;
                }

                const auto parsed = MethodResponseTraits<Response>::parse(method.arguments);
                if (!parsed.ok() || !parsed.value.has_value())
                {
                    return ResponseReaderError{
                        .code = ResponseReaderErrorCode::ParseFailed,
                        .message =
                            parsed.error.value_or("Failed to parse the JMAP method response"),
                        .methodError = std::nullopt,
                    };
                }

                return *parsed.value;
            }

            for (const auto& method : methods)
            {
                if (method.name != "error")
                {
                    continue;
                }

                const auto parsedError = parseMethodError(method.arguments);
                return ResponseReaderError{
                    .code = ResponseReaderErrorCode::MethodError,
                    .message = parsedError.ok() && parsedError.value.has_value()
                                   ? parsedError.value->description.value_or(parsedError.value->type)
                                   : "The JMAP method call returned an error response",
                    .methodError = parsedError.ok() ? parsedError.value : std::nullopt,
                };
            }

            return ResponseReaderError{
                .code = ResponseReaderErrorCode::UnexpectedMethodName,
                .message = "The JMAP response method name did not match the expected type",
                .methodError = std::nullopt,
            };
        }

        template <typename Response>
        [[nodiscard]] ResponseReadResult<Response> require(const CallHandle<Response>& handle) const
        {
            return get(handle);
        }

      private:
        const ResponseEnvelope& m_envelope;
    };

} // namespace javelin::jmap::api
