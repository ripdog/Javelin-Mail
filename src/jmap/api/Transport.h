#pragma once

#include "jmap/api/Cancellation.h"
#include "jmap/api/Error.h"

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

    struct HttpRequest
    {
        HttpMethod method = HttpMethod::Get;
        QUrl url;
        QList<HttpHeader> headers;
        QByteArray body;
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
