#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <variant>

namespace javelin::jmap::api
{

    class JmapMethodTransport;

    struct ApiRequestContext
    {
        javelin::jmap::auth::AccountCredentials credentials;
        std::string apiUrl;
        JmapTransportPolicy transportPolicy = JmapTransportPolicy::Preferred;
    };

    using MethodCallerResult =
        std::variant<ResponseEnvelope, TransportError, AuthError, ProtocolError>;

    class MethodCaller
    {
      public:
        MethodCaller(JmapMethodTransport& transport,
                     const javelin::jmap::auth::TokenRefresher* tokenRefresher = nullptr,
                     javelin::jmap::auth::SecretStore* secretStore = nullptr);

        [[nodiscard]] QCoro::Task<MethodCallerResult>
        call(ApiRequestContext requestContext, RequestEnvelope request,
             CancellationToken cancellation = {}) const;
        [[nodiscard]] QCoro::Task<MethodCallerResult>
        call(ApiRequestContext requestContext, RequestBuilder request,
             CancellationToken cancellation = {}) const;

      private:
        JmapMethodTransport& m_transport;
        const javelin::jmap::auth::TokenRefresher* m_tokenRefresher;
        javelin::jmap::auth::SecretStore* m_secretStore;
    };

} // namespace javelin::jmap::api
