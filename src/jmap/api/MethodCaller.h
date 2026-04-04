#pragma once

#include "jmap/api/Error.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <variant>

namespace javelin::jmap::api
{

    class AbstractTransport;

    struct ApiRequestContext
    {
        javelin::jmap::auth::AccountCredentials credentials;
        std::string apiUrl;
    };

    using MethodCallerResult =
        std::variant<ResponseEnvelope, TransportError, AuthError, ProtocolError>;

    class MethodCaller
    {
      public:
        MethodCaller(AbstractTransport& transport,
                     const javelin::jmap::auth::TokenRefresher* tokenRefresher = nullptr,
                     javelin::jmap::auth::SecretStore* secretStore = nullptr);

        [[nodiscard]] QCoro::Task<MethodCallerResult> call(const ApiRequestContext& requestContext,
                                                           const RequestEnvelope& request) const;

      private:
        AbstractTransport& m_transport;
        const javelin::jmap::auth::TokenRefresher* m_tokenRefresher;
        javelin::jmap::auth::SecretStore* m_secretStore;
    };

} // namespace javelin::jmap::api
