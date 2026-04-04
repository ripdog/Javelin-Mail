#include "jmap/api/MethodCaller.h"

#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace javelin::jmap::api
{

    namespace
    {

        [[nodiscard]] HttpRequest buildApiRequest(const ApiRequestContext& requestContext,
                                                  const std::string& accessToken,
                                                  const std::string& body)
        {
            return HttpRequest{
                .method = HttpMethod::Post,
                .url = QUrl{QString::fromStdString(requestContext.apiUrl)},
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
                        HttpHeader{
                            .name = "Content-Type",
                            .value = "application/json",
                        },
                    },
                .body = QByteArray::fromStdString(body),
            };
        }

    } // namespace

    MethodCaller::MethodCaller(AbstractTransport& transport,
                               const javelin::jmap::auth::TokenRefresher* tokenRefresher,
                               javelin::jmap::auth::SecretStore* secretStore)
        : m_transport(transport), m_tokenRefresher(tokenRefresher), m_secretStore(secretStore)
    {
    }

    QCoro::Task<MethodCallerResult> MethodCaller::call(const ApiRequestContext& requestContext,
                                                       const RequestEnvelope& request) const
    {
        const auto serializedRequest = serializeRequestEnvelope(request);
        if (!serializedRequest.has_value())
        {
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = "Failed to serialize JMAP request envelope",
            };
        }

        const javelin::jmap::auth::AccessTokenResolver accessTokenResolver{m_tokenRefresher,
                                                                           m_secretStore};
        const auto tokenResult = accessTokenResolver.resolve(requestContext.credentials);
        if (std::holds_alternative<AuthError>(tokenResult))
        {
            co_return std::get<AuthError>(tokenResult);
        }

        const auto transportResult = co_await m_transport.send(buildApiRequest(
            requestContext, std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken,
            *serializedRequest));
        if (std::holds_alternative<TransportError>(transportResult))
        {
            co_return std::get<TransportError>(transportResult);
        }

        const auto response = std::get<HttpResponse>(transportResult);
        const auto parseResult = parseResponseEnvelope(response.body.toStdString());
        if (!parseResult.ok())
        {
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = *parseResult.error,
            };
        }

        co_return *parseResult.value;
    }

} // namespace javelin::jmap::api
