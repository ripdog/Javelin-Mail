#pragma once

#include "jmap/api/Error.h"

#include <QCoroTask>

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QPair>
#include <QUrl>

#include <variant>

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

        [[nodiscard]] virtual QCoro::Task<TransportResult> send(const HttpRequest& request) = 0;
    };

    class QtNetworkTransport : public AbstractTransport
    {
      public:
        explicit QtNetworkTransport(QNetworkAccessManager& networkAccessManager);
        ~QtNetworkTransport() override = default;

        [[nodiscard]] QCoro::Task<TransportResult> send(const HttpRequest& request) override;

      private:
        QNetworkAccessManager& m_networkAccessManager;
    };

} // namespace javelin::jmap::api
