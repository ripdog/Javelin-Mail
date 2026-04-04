#include "jmap/api/SessionClient.h"

#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"

#include <QByteArray>

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
    SessionClient::discover(const javelin::jmap::auth::SessionRequestContext& requestContext) const
    {
        using namespace std::chrono_literals;

        javelin::jmap::auth::OAuthToken token = requestContext.credentials.token;
        if (!token.hasAccessToken())
        {
            co_return AuthError{
                .code = AuthErrorCode::MissingAccessToken,
                .message = "Session discovery requires an access token",
            };
        }

        if (token.isExpired(javelin::jmap::auth::Clock::now(), 60s))
        {
            if (!token.hasRefreshToken())
            {
                co_return AuthError{
                    .code = AuthErrorCode::MissingRefreshToken,
                    .message = "Access token is expired and no refresh token is available",
                };
            }

            if (m_tokenRefresher == nullptr)
            {
                co_return AuthError{
                    .code = AuthErrorCode::TokenExpired,
                    .message = "Access token is expired and no token refresher is configured",
                };
            }

            const auto refreshResult = m_tokenRefresher->refresh(requestContext.credentials);
            if (std::holds_alternative<AuthError>(refreshResult))
            {
                co_return std::get<AuthError>(refreshResult);
            }

            token = std::get<javelin::jmap::auth::OAuthToken>(refreshResult);

            if (m_secretStore != nullptr &&
                !m_secretStore->store(requestContext.credentials.accountId, token))
            {
                co_return AuthError{
                    .code = AuthErrorCode::SecretStoreFailure,
                    .message = "Failed to persist refreshed token",
                };
            }
        }

        const auto transportResult = co_await m_transport.send(
            buildSessionRequest(QUrl{QString::fromStdString(requestContext.credentials.sessionUrl)},
                                token.accessToken));

        if (std::holds_alternative<TransportError>(transportResult))
        {
            co_return std::get<TransportError>(transportResult);
        }

        const HttpResponse& response = std::get<HttpResponse>(transportResult);
        const auto parseResult =
            parseSession(response.body.toStdString(), requestContext.requiredCapabilities);
        if (!parseResult.ok())
        {
            const SessionParseError& error = *parseResult.error;
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
