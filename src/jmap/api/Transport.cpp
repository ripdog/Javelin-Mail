#include "jmap/api/Transport.h"

#include "jmap/api/MethodEnvelope.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <QCoroNetworkReply>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <QDebug>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QStringList>

namespace javelin::jmap::api
{
    Q_LOGGING_CATEGORY(logTransport, "jmap.transport")

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

        [[nodiscard]] QString
        summarizeInvocations(const std::vector<javelin::jmap::api::MethodInvocation>& invocations)
        {
            QStringList methods;
            methods.reserve(static_cast<qsizetype>(invocations.size()));
            for (const auto& invocation : invocations)
            {
                methods.push_back(
                    QStringLiteral("%1#%2").arg(QString::fromStdString(invocation.name),
                                                QString::fromStdString(invocation.callId)));
            }
            return methods.join(QStringLiteral(","));
        }

        [[nodiscard]] QString summarizeJmapRequest(const QByteArray& body)
        {
            const auto parsed = javelin::jmap::api::parseRequestEnvelope(body.toStdString());
            if (!parsed.value.has_value())
            {
                return {};
            }

            return QStringLiteral("methods %1")
                .arg(summarizeInvocations(parsed.value->methodCalls));
        }

        [[nodiscard]] QString summarizeJmapResponse(const QByteArray& body)
        {
            const auto parsed = javelin::jmap::api::parseResponseEnvelope(body.toStdString());
            if (!parsed.value.has_value())
            {
                return {};
            }

            return QStringLiteral("methods %1")
                .arg(summarizeInvocations(parsed.value->methodResponses));
        }

        [[nodiscard]] QStringList failedJmapCalls(const QByteArray& body)
        {
            const auto parsed = javelin::jmap::api::parseResponseEnvelope(body.toStdString());
            QStringList failures;
            if (!parsed.value.has_value())
                return failures;
            for (const auto& response : parsed.value->methodResponses)
            {
                if (response.name == "error")
                    failures.push_back(QString::fromStdString(response.callId));
            }
            return failures;
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
                .code = statusCode.isValid() ? TransportErrorCode::HttpFailure
                                             : TransportErrorCode::NetworkFailure,
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

    QCoro::Task<TransportResult> QtNetworkTransport::send(HttpRequest request)
    {
        QNetworkRequest networkRequest{request.url};
        networkRequest.setTransferTimeout(requestTimeoutMs);
        for (const HttpHeader& header : request.headers)
        {
            networkRequest.setRawHeader(header.name, header.value);
        }

        const auto requestSummary =
            request.method == HttpMethod::Post ? summarizeJmapRequest(request.body) : QString{};
        qCInfo(logTransport).noquote()
            << "request" << request.url.toString()
            << (request.method == HttpMethod::Get ? "GET" : "POST") << requestSummary;

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
            qCWarning(logTransport).noquote() << "network failure" << request.url.toString()
                                              << reply->error() << reply->errorString();
            if (reply->error() == QNetworkReply::TimeoutError)
            {
                // A machine sleep can leave Qt's pooled HTTP connections unusable even after the
                // network is available again. Do not replay the request because a timed-out POST
                // may have reached the server, but ensure the next request opens a fresh
                // connection.
                m_networkAccessManager.clearConnectionCache();
            }
            co_return mapReplyError(*reply);
        }

        const auto statusCodeAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int statusCode = statusCodeAttribute.isValid() ? statusCodeAttribute.toInt() : 0;
        const QByteArray responseBody = reply->readAll();
        const auto responseSummary =
            request.method == HttpMethod::Post ? summarizeJmapResponse(responseBody) : QString{};
        const auto failedCalls =
            request.method == HttpMethod::Post ? failedJmapCalls(responseBody) : QStringList{};
        if (failedCalls.isEmpty())
            qCInfo(logTransport).noquote() << "success" << statusCode << responseSummary;
        else
            qCWarning(logTransport).noquote() << "JMAP method failure" << statusCode << "calls"
                                              << failedCalls.join(QLatin1Char(','));
        if (statusCode >= 400)
        {
            qCWarning(logTransport).noquote() << "HTTP failure" << request.url.toString()
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
