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
#include <QFile>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QScopeGuard>

#include <chrono>

namespace javelin::jmap::api
{
    Q_LOGGING_CATEGORY(logTransport, "jmap.transport")

    namespace
    {
        constexpr int requestTimeoutMs = 30000;
        constexpr auto networkInvalidatedProperty = "javelinNetworkInvalidated";

        [[nodiscard]] QByteArray summarizeBody(const QByteArray& body)
        {
            constexpr int maxBytes = 512;
            if (body.size() <= maxBytes)
            {
                return body;
            }

            return body.first(maxBytes) + "...";
        }

        [[nodiscard]] std::optional<std::chrono::seconds> retryAfter(QNetworkReply& reply)
        {
            bool parsed = false;
            const auto seconds =
                reply.rawHeader(QByteArrayLiteral("Retry-After")).toLongLong(&parsed);
            if (!parsed || seconds < 0)
                return std::nullopt;
            return std::chrono::seconds{seconds};
        }

        template <typename Response>
        [[nodiscard]] bool isUnauthorized(const std::variant<Response, TransportError>& result)
        {
            const auto* error = std::get_if<TransportError>(&result);
            return error != nullptr && error->code == TransportErrorCode::HttpFailure &&
                   error->httpStatus == 401;
        }

        [[nodiscard]] TransportError localIoError(const QString& operation, const QFile& file)
        {
            return TransportError{
                .code = TransportErrorCode::LocalIoFailure,
                .message =
                    QStringLiteral("%1: %2").arg(operation, file.errorString()).toStdString(),
                .httpStatus = std::nullopt,
                .networkError = std::nullopt,
                .retryAfter = std::nullopt,
            };
        }

        void replaceBearerToken(HttpRequest& request, const std::string& accessToken)
        {
            const auto value =
                QByteArrayLiteral("Bearer ") + QByteArray::fromStdString(accessToken);
            for (auto& header : request.headers)
            {
                if (header.name.compare(QByteArrayLiteral("Authorization"), Qt::CaseInsensitive) ==
                    0)
                {
                    header.value = value;
                    break;
                }
            }
            request.authentication->accessToken = accessToken;
        }

        [[nodiscard]] TransportError mapReplyError(QNetworkReply& reply)
        {
            if (reply.property(networkInvalidatedProperty).toBool())
            {
                return TransportError{
                    .code = TransportErrorCode::NetworkFailure,
                    .message = "Network changed while the HTTP request was in flight",
                    .httpStatus = std::nullopt,
                    .networkError = static_cast<int>(reply.error()),
                    .retryAfter = std::nullopt,
                };
            }
            if (reply.error() == QNetworkReply::OperationCanceledError)
            {
                return TransportError{
                    .code = TransportErrorCode::Cancelled,
                    .message = reply.errorString().toStdString(),
                    .httpStatus = std::nullopt,
                    .networkError = static_cast<int>(reply.error()),
                    .retryAfter = std::nullopt,
                };
            }

            const auto statusCode = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute);
            return TransportError{
                .code = statusCode.isValid() ? TransportErrorCode::HttpFailure
                                             : TransportErrorCode::NetworkFailure,
                .message = reply.errorString().toStdString(),
                .httpStatus =
                    statusCode.isValid() ? std::optional{statusCode.toInt()} : std::nullopt,
                .networkError = static_cast<int>(reply.error()),
                .retryAfter = retryAfter(reply),
            };
        }

    } // namespace

    QCoro::Task<FileTransportResult> AbstractTransport::sendToFile(HttpRequest request,
                                                                   QString filePath)
    {
        auto result = co_await send(std::move(request));
        if (const auto* error = std::get_if<TransportError>(&result))
            co_return *error;

        auto response = std::get<HttpResponse>(std::move(result));
        QFile file{filePath};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            co_return localIoError(QStringLiteral("Open streamed HTTP response"), file);
        if (file.write(response.body) != response.body.size())
            co_return localIoError(QStringLiteral("Write streamed HTTP response"), file);
        if (!file.flush())
            co_return localIoError(QStringLiteral("Flush streamed HTTP response"), file);
        co_return HttpFileResponse{.statusCode = response.statusCode,
                                   .size = static_cast<std::uint64_t>(response.body.size())};
    }

    RefreshingTransport::RefreshingTransport(AbstractTransport& transport) : m_transport(transport)
    {
    }

    void
    RefreshingTransport::setAccessTokenProvider(javelin::jmap::auth::AccessTokenProvider provider)
    {
        m_accessTokenProvider = std::move(provider);
    }

    void
    RefreshingTransport::setRefreshHandler(javelin::jmap::auth::AccessTokenRefreshHandler handler)
    {
        m_refreshHandler = std::move(handler);
    }

    void RefreshingTransport::invalidateConnections()
    {
        m_transport.invalidateConnections();
    }

    QCoro::Task<TransportResult> RefreshingTransport::send(HttpRequest request)
    {
        if (request.authentication.has_value() && m_accessTokenProvider)
        {
            const auto currentAccessToken =
                m_accessTokenProvider(request.authentication->accountId);
            if (currentAccessToken.has_value() &&
                *currentAccessToken != request.authentication->accessToken)
            {
                replaceBearerToken(request, *currentAccessToken);
            }
        }

        auto retryRequest = request;
        auto result = co_await m_transport.send(std::move(request));
        if (!isUnauthorized(result) || !retryRequest.authentication.has_value() ||
            !m_refreshHandler)
        {
            co_return result;
        }

        const auto rejectedAccessToken = retryRequest.authentication->accessToken;
        auto refreshedAccessToken =
            co_await m_refreshHandler(retryRequest.authentication->accountId, rejectedAccessToken);
        if (!refreshedAccessToken.has_value() || *refreshedAccessToken == rejectedAccessToken)
        {
            co_return result;
        }
        if (retryRequest.cancellation.isCancellationRequested())
        {
            co_return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = "HTTP request cancelled while refreshing authentication",
                .httpStatus = std::nullopt,
            };
        }

        replaceBearerToken(retryRequest, *refreshedAccessToken);
        retryRequest.dispatched = {};
        co_return co_await m_transport.send(std::move(retryRequest));
    }

    QCoro::Task<FileTransportResult> RefreshingTransport::sendToFile(HttpRequest request,
                                                                     QString filePath)
    {
        if (request.authentication.has_value() && m_accessTokenProvider)
        {
            const auto currentAccessToken =
                m_accessTokenProvider(request.authentication->accountId);
            if (currentAccessToken.has_value() &&
                *currentAccessToken != request.authentication->accessToken)
            {
                replaceBearerToken(request, *currentAccessToken);
            }
        }

        auto retryRequest = request;
        auto result = co_await m_transport.sendToFile(std::move(request), filePath);
        if (!isUnauthorized(result) || !retryRequest.authentication.has_value() ||
            !m_refreshHandler)
        {
            co_return result;
        }

        const auto rejectedAccessToken = retryRequest.authentication->accessToken;
        auto refreshedAccessToken =
            co_await m_refreshHandler(retryRequest.authentication->accountId, rejectedAccessToken);
        if (!refreshedAccessToken.has_value() || *refreshedAccessToken == rejectedAccessToken)
        {
            co_return result;
        }
        if (retryRequest.cancellation.isCancellationRequested())
        {
            co_return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = "HTTP request cancelled while refreshing authentication",
                .httpStatus = std::nullopt,
            };
        }

        replaceBearerToken(retryRequest, *refreshedAccessToken);
        retryRequest.dispatched = {};
        co_return co_await m_transport.sendToFile(std::move(retryRequest), std::move(filePath));
    }

    QtNetworkTransport::QtNetworkTransport(QNetworkAccessManager& networkAccessManager)
        : m_networkAccessManager(networkAccessManager)
    {
    }

    void QtNetworkTransport::invalidateConnections()
    {
        std::vector<QPointer<QNetworkReply>> activeReplies;
        activeReplies.swap(m_activeReplies);
        for (const auto& reply : activeReplies)
        {
            if (auto* activeReply = reply.data(); activeReply != nullptr)
            {
                activeReply->setProperty(networkInvalidatedProperty, true);
            }
            if (auto* activeReply = reply.data(); activeReply != nullptr)
            {
                activeReply->abort();
            }
        }
        m_networkAccessManager.clearConnectionCache();
    }

    QCoro::Task<TransportResult> QtNetworkTransport::send(HttpRequest request)
    {
        if (request.cancellation.isCancellationRequested())
        {
            co_return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = "HTTP request cancelled before dispatch",
                .httpStatus = std::nullopt,
            };
        }

        QNetworkRequest networkRequest{request.url};
        networkRequest.setTransferTimeout(requestTimeoutMs);
        for (const HttpHeader& header : request.headers)
        {
            networkRequest.setRawHeader(header.name, header.value);
        }

        qCInfo(logTransport).noquote() << "request" << request.url.toString()
                                       << (request.method == HttpMethod::Get ? "GET" : "POST");

        QNetworkReply* reply = nullptr;
        switch (request.method)
        {
        case HttpMethod::Get:
            reply = m_networkAccessManager.get(networkRequest);
            break;
        case HttpMethod::Post:
            reply = m_networkAccessManager.post(networkRequest, request.body);
            break;
        }
        m_activeReplies.emplace_back(reply);
        const auto unregisterReply = qScopeGuard(
            [this, reply]()
            {
                std::erase_if(m_activeReplies, [reply](const QPointer<QNetworkReply>& active)
                              { return active.isNull() || active.data() == reply; });
            });
        static_cast<void>(unregisterReply);
        if (request.dispatched)
            request.dispatched();

        const auto cancellationRegistration = request.cancellation.registerCallback(
            [reply = QPointer<QNetworkReply>{reply}]()
            {
                if (reply.isNull())
                {
                    return;
                }
                auto* activeReply = reply.data();
                QMetaObject::invokeMethod(
                    activeReply,
                    [reply]()
                    {
                        if (auto* queuedReply = reply.data())
                        {
                            queuedReply->abort();
                        }
                    },
                    Qt::QueuedConnection);
            });
        static_cast<void>(cancellationRegistration);
        co_await qCoro(reply).waitForFinished();

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
        qCInfo(logTransport).noquote() << "success" << statusCode;
        if (statusCode >= 400)
        {
            qCWarning(logTransport).noquote() << "HTTP failure" << request.url.toString()
                                              << statusCode << summarizeBody(responseBody);
            co_return TransportError{
                .code = TransportErrorCode::HttpFailure,
                .message = reply->errorString().toStdString(),
                .httpStatus = statusCode,
                .networkError = static_cast<int>(reply->error()),
                .retryAfter = retryAfter(*reply),
            };
        }

        co_return HttpResponse{
            .statusCode = statusCode,
            .body = responseBody,
        };
    }

    QCoro::Task<FileTransportResult> QtNetworkTransport::sendToFile(HttpRequest request,
                                                                    QString filePath)
    {
        if (request.cancellation.isCancellationRequested())
        {
            co_return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = "HTTP request cancelled before dispatch",
                .httpStatus = std::nullopt,
            };
        }

        QFile file{filePath};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            co_return localIoError(QStringLiteral("Open streamed HTTP response"), file);
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        QNetworkRequest networkRequest{request.url};
        networkRequest.setTransferTimeout(requestTimeoutMs);
        for (const HttpHeader& header : request.headers)
            networkRequest.setRawHeader(header.name, header.value);

        qCDebug(logTransport).noquote()
            << "request" << request.url.toString()
            << (request.method == HttpMethod::Get ? "GET" : "POST") << "streamed";

        QNetworkReply* reply = nullptr;
        switch (request.method)
        {
        case HttpMethod::Get:
            reply = m_networkAccessManager.get(networkRequest);
            break;
        case HttpMethod::Post:
            reply = m_networkAccessManager.post(networkRequest, request.body);
            break;
        }
        reply->setReadBufferSize(256 * 1024);
        m_activeReplies.emplace_back(reply);
        const auto unregisterReply = qScopeGuard(
            [this, reply]()
            {
                std::erase_if(m_activeReplies, [reply](const QPointer<QNetworkReply>& active)
                              { return active.isNull() || active.data() == reply; });
            });
        static_cast<void>(unregisterReply);
        if (request.dispatched)
            request.dispatched();

        std::optional<TransportError> writeError;
        const auto drainReply = [&file, reply, &writeError]()
        {
            constexpr qint64 chunkSize = 64 * 1024;
            while (!writeError.has_value() && reply->bytesAvailable() > 0)
            {
                const QByteArray chunk = reply->read(chunkSize);
                if (chunk.isEmpty())
                    break;
                if (file.write(chunk) != chunk.size())
                {
                    writeError = localIoError(QStringLiteral("Write streamed HTTP response"), file);
                    reply->abort();
                }
            }
        };
        const auto readyReadConnection =
            QObject::connect(reply, &QIODevice::readyRead, reply, drainReply);
        static_cast<void>(readyReadConnection);
        drainReply();

        const auto cancellationRegistration = request.cancellation.registerCallback(
            [reply = QPointer<QNetworkReply>{reply}]()
            {
                if (reply.isNull())
                    return;
                auto* activeReply = reply.data();
                QMetaObject::invokeMethod(
                    activeReply,
                    [reply]()
                    {
                        if (auto* queuedReply = reply.data())
                            queuedReply->abort();
                    },
                    Qt::QueuedConnection);
            });
        static_cast<void>(cancellationRegistration);
        co_await qCoro(reply).waitForFinished();
        drainReply();

        const auto deleteReply = qScopeGuard(
            [reply]()
            {
                if (reply != nullptr)
                    reply->deleteLater();
            });

        if (writeError.has_value())
            co_return *writeError;
        if (!file.flush())
            co_return localIoError(QStringLiteral("Flush streamed HTTP response"), file);

        if (reply->error() != QNetworkReply::NoError)
        {
            qCWarning(logTransport).noquote() << "network failure" << request.url.toString()
                                              << reply->error() << reply->errorString();
            if (reply->error() == QNetworkReply::TimeoutError)
                m_networkAccessManager.clearConnectionCache();
            co_return mapReplyError(*reply);
        }

        const auto statusCodeAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int statusCode = statusCodeAttribute.isValid() ? statusCodeAttribute.toInt() : 0;
        if (statusCode >= 400)
        {
            qCWarning(logTransport).noquote()
                << "HTTP failure" << request.url.toString() << statusCode;
            co_return TransportError{
                .code = TransportErrorCode::HttpFailure,
                .message = reply->errorString().toStdString(),
                .httpStatus = statusCode,
                .networkError = static_cast<int>(reply->error()),
                .retryAfter = retryAfter(*reply),
            };
        }

        qCDebug(logTransport).noquote()
            << "success" << statusCode << "streamed" << static_cast<qulonglong>(file.size());
        co_return HttpFileResponse{.statusCode = statusCode,
                                   .size = static_cast<std::uint64_t>(file.size())};
    }

} // namespace javelin::jmap::api
