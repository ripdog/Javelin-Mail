#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"
#include "jmap/auth/Auth.h"

#include <QCoroTask>

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QPair>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <variant>
#include <vector>

namespace javelin::jmap::api
{

    enum class HttpMethod
    {
        Get,
        Post,
    };

    struct HttpHeader
    {
        QByteArray name;
        QByteArray value;
    };

    struct BearerAuthentication
    {
        std::string accountId;
        std::string accessToken;
    };

    struct HttpRequest
    {
        HttpMethod method = HttpMethod::Get;
        QUrl url;
        QList<HttpHeader> headers;
        QByteArray body;
        std::optional<BearerAuthentication> authentication;
        CancellationToken cancellation{};
        std::function<void()> dispatched;
    };

    struct HttpResponse
    {
        int statusCode = 0;
        QByteArray body;
    };

    using TransportResult = std::variant<HttpResponse, TransportError>;

    class AbstractTransport
    {
      public:
        virtual ~AbstractTransport() = default;

        virtual void invalidateConnections()
        {
        }

        [[nodiscard]] virtual QCoro::Task<TransportResult> send(HttpRequest request) = 0;
    };

    class RefreshingTransport final : public AbstractTransport
    {
      public:
        explicit RefreshingTransport(AbstractTransport& transport);

        void setAccessTokenProvider(javelin::jmap::auth::AccessTokenProvider provider);
        void setRefreshHandler(javelin::jmap::auth::AccessTokenRefreshHandler handler);
        void invalidateConnections() override;

        [[nodiscard]] QCoro::Task<TransportResult> send(HttpRequest request) override;

      private:
        AbstractTransport& m_transport;
        javelin::jmap::auth::AccessTokenProvider m_accessTokenProvider;
        javelin::jmap::auth::AccessTokenRefreshHandler m_refreshHandler;
    };

    class QtNetworkTransport : public AbstractTransport
    {
      public:
        explicit QtNetworkTransport(QNetworkAccessManager& networkAccessManager);
        ~QtNetworkTransport() override = default;

        void invalidateConnections() override;

        [[nodiscard]] QCoro::Task<TransportResult> send(HttpRequest request) override;

      private:
        QNetworkAccessManager& m_networkAccessManager;
        std::vector<QPointer<QNetworkReply>> m_activeReplies;
    };

} // namespace javelin::jmap::api
