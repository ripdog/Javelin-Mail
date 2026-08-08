#include "jmap/api/MethodCaller.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/auth/AccessTokenResolver.h"

#include <QDebug>
#include <QString>

namespace javelin::jmap::api
{

    MethodCaller::MethodCaller(JmapMethodTransport& transport,
                               const javelin::jmap::auth::TokenRefresher* tokenRefresher,
                               javelin::jmap::auth::SecretStore* secretStore)
        : m_transport(transport), m_tokenRefresher(tokenRefresher), m_secretStore(secretStore)
    {
    }

    QCoro::Task<MethodCallerResult> MethodCaller::call(ApiRequestContext requestContext,
                                                       RequestEnvelope request,
                                                       CancellationToken cancellation,
                                                       std::function<void()> dispatched) const
    {
        if (requestContext.requestLimits.has_value())
        {
            const auto& limits = *requestContext.requestLimits;
            if (request.methodCalls.size() > limits.maxCallsInRequest)
            {
                co_return ProtocolError{
                    .code = ProtocolErrorCode::InvalidRequest,
                    .message = "JMAP request exceeds the negotiated maxCallsInRequest limit",
                };
            }
            const auto encoded = serializeRequestEnvelope(request);
            if (!encoded.has_value())
            {
                co_return ProtocolError{
                    .code = ProtocolErrorCode::InvalidRequest,
                    .message = "JMAP request could not be encoded",
                };
            }
            if (encoded->size() > limits.maxSizeRequest)
            {
                co_return ProtocolError{
                    .code = ProtocolErrorCode::InvalidRequest,
                    .message = "JMAP request exceeds the negotiated maxSizeRequest limit",
                };
            }
        }
        if (cancellation.isCancellationRequested())
        {
            co_return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = "JMAP method call cancelled before dispatch",
                .httpStatus = std::nullopt,
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

        const auto transportResult = co_await m_transport.call(JmapMethodRequest{
            .accountId = requestContext.credentials.accountId,
            .apiUrl = std::move(requestContext.apiUrl),
            .accessToken = std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken,
            .envelope = std::move(request),
            .cancellation = std::move(cancellation),
            .transportPolicy = requestContext.transportPolicy,
            .dispatched = std::move(dispatched),
        });
        if (std::holds_alternative<TransportError>(transportResult))
        {
            const auto& error = std::get<TransportError>(transportResult);
            qWarning().noquote() << "JMAP method call transport failure"
                                 << QString::fromStdString(error.message);
            co_return std::get<TransportError>(transportResult);
        }

        if (const auto* error = std::get_if<ProtocolError>(&transportResult))
        {
            qWarning().noquote() << "JMAP method call protocol failure"
                                 << QString::fromStdString(error->message);
            co_return *error;
        }
        co_return std::get<ResponseEnvelope>(transportResult);
    }

    QCoro::Task<MethodCallerResult> MethodCaller::call(ApiRequestContext requestContext,
                                                       RequestBuilder request,
                                                       CancellationToken cancellation,
                                                       std::function<void()> dispatched) const
    {
        co_return co_await call(requestContext, request.build(), std::move(cancellation),
                                std::move(dispatched));
    }

} // namespace javelin::jmap::api
