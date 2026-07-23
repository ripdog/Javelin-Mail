#pragma once

#include "jmap/api/MethodEnvelope.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace javelin::jmap::api
{

    template <typename Response> struct MethodRequest
    {
        std::string name;
        std::string arguments;
    };

    template <typename Response> struct CallHandle
    {
        std::string callId;
    };

    class RequestBuilder
    {
      public:
        RequestBuilder& useCore()
        {
            return useCapability("urn:ietf:params:jmap:core");
        }

        RequestBuilder& useMail()
        {
            return useCapability("urn:ietf:params:jmap:mail");
        }

        RequestBuilder& useCapability(std::string capability)
        {
            m_usingCapabilities.push_back(std::move(capability));
            return *this;
        }

        RequestBuilder& useCapabilities(std::vector<std::string> capabilities)
        {
            for (auto& capability : capabilities)
            {
                m_usingCapabilities.push_back(std::move(capability));
            }
            return *this;
        }

        template <typename Response>
        [[nodiscard]] CallHandle<Response> call(const MethodRequest<Response>& request,
                                                std::optional<std::string> callId = std::nullopt)
        {
            const auto resolvedCallId =
                callId.has_value() ? std::move(*callId)
                                   : std::string{"call-"} + std::to_string(++m_nextCallNumber);
            m_methodCalls.push_back(MethodInvocation{
                .name = request.name,
                .arguments = request.arguments,
                .callId = resolvedCallId,
            });
            return CallHandle<Response>{.callId = std::move(resolvedCallId)};
        }

        [[nodiscard]] RequestEnvelope build() const
        {
            return RequestEnvelope{
                .usingCapabilities = m_usingCapabilities,
                .methodCalls = m_methodCalls,
                .createdIds = std::nullopt,
            };
        }

      private:
        std::vector<std::string> m_usingCapabilities;
        std::vector<MethodInvocation> m_methodCalls;
        std::size_t m_nextCallNumber = 0;
    };

} // namespace javelin::jmap::api
