#pragma once

#include "jmap/api/Error.h"
#include "jmap/api/Session.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <variant>

namespace javelin::jmap::api
{

    class AbstractTransport;

    using SessionClientResult = std::variant<Session, TransportError, AuthError, ProtocolError>;

    class SessionClient
    {
      public:
        SessionClient(AbstractTransport& transport,
                      const javelin::jmap::auth::TokenRefresher* tokenRefresher = nullptr,
                      javelin::jmap::auth::SecretStore* secretStore = nullptr);

        [[nodiscard]] QCoro::Task<SessionClientResult>
        discover(javelin::jmap::auth::SessionRequestContext requestContext) const;
        [[nodiscard]] std::string resolvedSessionUrl() const;

      private:
        AbstractTransport& m_transport;
        const javelin::jmap::auth::TokenRefresher* m_tokenRefresher;
        javelin::jmap::auth::SecretStore* m_secretStore;
        mutable std::string m_resolvedSessionUrl;
    };

} // namespace javelin::jmap::api
