#include "jmap/api/Transport.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>

namespace javelin::jmap::api
{

    namespace
    {

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
        for (const HttpHeader& header : request.headers)
        {
            networkRequest.setRawHeader(header.name, header.value);
        }

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
            co_return mapReplyError(*reply);
        }

        const auto statusCodeAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int statusCode = statusCodeAttribute.isValid() ? statusCodeAttribute.toInt() : 0;
        if (statusCode >= 400)
        {
            co_return TransportError{
                .code = TransportErrorCode::HttpFailure,
                .message = reply->errorString().toStdString(),
                .httpStatus = statusCode,
            };
        }

        co_return HttpResponse{
            .statusCode = statusCode,
            .body = reply->readAll(),
        };
    }

} // namespace javelin::jmap::api
