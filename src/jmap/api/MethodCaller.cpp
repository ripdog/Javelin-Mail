#include "jmap/api/MethodCaller.h"

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccessTokenResolver.h"

#include <QDebug>
#include <QString>

namespace javelin::jmap::api
{

    MethodCaller::~MethodCaller() = default;

    MethodCaller::MethodCaller(AbstractTransport& transport,
                               const javelin::jmap::auth::TokenRefresher* tokenRefresher,
                               javelin::jmap::auth::SecretStore* secretStore)
        : m_ownedTransport(std::make_unique<HttpJmapMethodTransport>(transport)),
          m_transport(m_ownedTransport.get()), m_tokenRefresher(tokenRefresher),
          m_secretStore(secretStore)
    {
    }

    MethodCaller::MethodCaller(JmapMethodTransport& transport,
                               const javelin::jmap::auth::TokenRefresher* tokenRefresher,
                               javelin::jmap::auth::SecretStore* secretStore)
        : m_transport(&transport), m_tokenRefresher(tokenRefresher), m_secretStore(secretStore)
    {
    }

    QCoro::Task<MethodCallerResult> MethodCaller::call(ApiRequestContext requestContext,
                                                       RequestEnvelope request) const
    {
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

        const auto transportResult = co_await m_transport->call(JmapMethodRequest{
            .apiUrl = std::move(requestContext.apiUrl),
            .accessToken = std::get<javelin::jmap::auth::OAuthToken>(tokenResult).accessToken,
            .envelope = std::move(request),
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
            qWarning().noquote() << "JMAP method call response parse failure"
                                 << QString::fromStdString(error->message);
            co_return *error;
        }
        co_return std::get<ResponseEnvelope>(transportResult);
    }

    QCoro::Task<MethodCallerResult> MethodCaller::call(ApiRequestContext requestContext,
                                                       RequestBuilder request) const
    {
        co_return co_await call(requestContext, request.build());
    }

} // namespace javelin::jmap::api
