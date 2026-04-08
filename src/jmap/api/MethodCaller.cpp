#include "jmap/api/MethodCaller.h"

#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"

#include <QByteArray>
#include <QDebug>
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
        qInfo().noquote() << "JMAP method call start"
                          << QString::fromStdString(requestContext.apiUrl)
                          << static_cast<int>(request.methodCalls.size());
        const auto serializedRequest = serializeRequestEnvelope(request);
        if (!serializedRequest.has_value())
        {
            qWarning() << "JMAP method call serialization failure";
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
            const auto& error = std::get<AuthError>(tokenResult);
            qWarning().noquote() << "JMAP method call auth failure"
                                 << QString::fromStdString(error.message);
            co_return std::get<AuthError>(tokenResult);
        }

        const auto transportResult = co_await m_transport.send(buildApiRequest(
            requestContext, std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken,
            *serializedRequest));
        if (std::holds_alternative<TransportError>(transportResult))
        {
            const auto& error = std::get<TransportError>(transportResult);
            qWarning().noquote() << "JMAP method call transport failure"
                                 << QString::fromStdString(error.message);
            co_return std::get<TransportError>(transportResult);
        }

        const auto response = std::get<HttpResponse>(transportResult);
        const auto parseResult = parseResponseEnvelope(response.body.toStdString());
        if (!parseResult.ok())
        {
            qWarning().noquote() << "JMAP method call response parse failure"
                                 << QString::fromStdString(*parseResult.error);
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = *parseResult.error,
            };
        }

        qInfo() << "JMAP method call success"
                << static_cast<int>(parseResult.value->methodResponses.size());
        co_return *parseResult.value;
    }

    QCoro::Task<MethodCallerResult> MethodCaller::call(const ApiRequestContext& requestContext,
                                                       const RequestBuilder& request) const
    {
        co_return co_await call(requestContext, request.build());
    }

} // namespace javelin::jmap::api
