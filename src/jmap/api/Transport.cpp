#include "jmap/api/Transport.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QDebug>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>

namespace javelin::jmap::api
{

    namespace
    {
        constexpr int requestTimeoutMs = 30000;

        [[nodiscard]] QByteArray summarizeBody(const QByteArray& body)
        {
            constexpr int maxBytes = 512;
            if (body.size() <= maxBytes)
            {
                return body;
            }

            return body.first(maxBytes) + "...";
        }

        [[nodiscard]] TransportError mapReplyError(QNetworkReply& reply)
        {
            if (reply.error() == QNetworkReply::OperationCanceledError)
            {
                return TransportError{
                    .code = TransportErrorCode::Cancelled,
                    .message = reply.errorString().toStdString(),
                    .httpStatus = std::nullopt,
                };
            }

            const auto statusCode = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
            return TransportError{
                .code = TransportErrorCode::NetworkFailure,
                .message = reply.errorString().toStdString(),
                .httpStatus =
                    statusCode.isValid() ? std::optional{statusCode.toInt()} : std::nullopt,
            };
        }

    } // namespace

    QtNetworkTransport::QtNetworkTransport(QNetworkAccessManager& networkAccessManager)
        : m_networkAccessManager(networkAccessManager)
    {
    }

    QCoro::Task<TransportResult> QtNetworkTransport::send(const HttpRequest& request)
    {
        QNetworkRequest networkRequest{request.url};
        networkRequest.setTransferTimeout(requestTimeoutMs);
        for (const HttpHeader& header : request.headers)
        {
            networkRequest.setRawHeader(header.name, header.value);
        }

        qInfo().noquote() << "JMAP transport request" << request.url.toString()
                          << (request.method == HttpMethod::Get ? "GET" : "POST");

        QNetworkReply* reply = nullptr;
        switch (request.method)
        {
        case HttpMethod::Get:
            reply = co_await m_networkAccessManager.get(networkRequest);
            break;
        case HttpMethod::Post:
            reply = co_await m_networkAccessManager.post(networkRequest, request.body);
            break;
        }

        const auto deleteReply = qScopeGuard(
            [reply]()
            {
                if (reply != nullptr)
                {
                    reply->deleteLater();
                }
            });

        if (reply->error() != QNetworkReply::NoError)
        {
            qWarning().noquote() << "JMAP transport network error" << request.url.toString()
                                 << reply->error() << reply->errorString();
            co_return mapReplyError(*reply);
        }

        const auto statusCodeAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int statusCode = statusCodeAttribute.isValid() ? statusCodeAttribute.toInt() : 0;
        const QByteArray responseBody = reply->readAll();
        qInfo().noquote() << "JMAP transport response" << request.url.toString() << statusCode
                          << summarizeBody(responseBody);
        if (statusCode >= 400)
        {
            qWarning().noquote() << "JMAP transport HTTP failure" << request.url.toString()
                                 << statusCode << summarizeBody(responseBody);
            co_return TransportError{
                .code = TransportErrorCode::HttpFailure,
                .message = reply->errorString().toStdString(),
                .httpStatus = statusCode,
            };
        }

        co_return HttpResponse{
            .statusCode = statusCode,
            .body = responseBody,
        };
    }

} // namespace javelin::jmap::api
