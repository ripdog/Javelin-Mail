#include "jmap/api/SessionClient.h"

#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"

#include <QByteArray>
#include <QDebug>

#include <string>

namespace javelin::jmap::api
{

    namespace
    {

        [[nodiscard]] HttpRequest buildSessionRequest(const QUrl& sessionUrl,
                                                      const std::string& accessToken)
        {
            return HttpRequest{
                .method = HttpMethod::Get,
                .url = sessionUrl,
                .headers =
                    {
                        HttpHeader{
                            .name = "Authorization",
                            .value = QByteArray{"Bearer "} + QByteArray::fromStdString(accessToken),
                        },
                        HttpHeader{
                            .name = "Accept",
                            .value = "application/json",
                        },
                    },
                .body = {},
            };
        }

    } // namespace

    SessionClient::SessionClient(AbstractTransport& transport,
                                 const javelin::jmap::auth::TokenRefresher* tokenRefresher,
                                 javelin::jmap::auth::SecretStore* secretStore)
        : m_transport(transport), m_tokenRefresher(tokenRefresher), m_secretStore(secretStore)
    {
    }

    QCoro::Task<SessionClientResult>
    SessionClient::discover(javelin::jmap::auth::SessionRequestContext requestContext) const
    {
        const javelin::jmap::auth::AccessTokenResolver accessTokenResolver{m_tokenRefresher,
                                                                           m_secretStore};
        const auto tokenResult = accessTokenResolver.resolve(requestContext.credentials);
        if (std::holds_alternative<AuthError>(tokenResult))
        {
            const auto& error = std::get<AuthError>(tokenResult);
            qWarning().noquote() << "JMAP session discovery auth failure"
                                 << QString::fromStdString(error.message);
            co_return std::get<AuthError>(tokenResult);
        }

        const auto transportResult = co_await m_transport.send(buildSessionRequest(
            QUrl{QString::fromStdString(requestContext.credentials.sessionUrl)},
            std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken));

        if (std::holds_alternative<TransportError>(transportResult))
        {
            const auto& error = std::get<TransportError>(transportResult);
            qWarning().noquote() << "JMAP session discovery transport failure"
                                 << QString::fromStdString(error.message);
            co_return std::get<TransportError>(transportResult);
        }

        const HttpResponse& response = std::get<HttpResponse>(transportResult);
        const auto parseResult =
            parseSession(response.body.toStdString(), requestContext.requiredCapabilities);
        if (!parseResult.ok())
        {
            const SessionParseError& error = *parseResult.error;
            qWarning().noquote() << "JMAP session discovery parse failure"
                                 << QString::fromStdString(error.message);
            co_return ProtocolError{
                .code = error.code == SessionParseErrorCode::CapabilityValidationFailed
                            ? ProtocolErrorCode::CapabilityNegotiationFailed
                            : ProtocolErrorCode::InvalidResponse,
                .message = error.message,
            };
        }

        co_return *parseResult.session;
    }

} // namespace javelin::jmap::api
