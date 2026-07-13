#include "jmap/api/JmapMethodTransport.h"

#include "jmap/api/Transport.h"
#include "jmap/cache/JmapTransportPreferenceRepository.h"

#include <QCoroSignal>

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <glaze/glaze.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace javelin::jmap::api::detail
{
    struct WebSocketMessageHeader
    {
        std::optional<std::string> type;
        std::optional<std::string> requestId;
        std::optional<std::string> title;
        std::optional<std::string> detail;
        std::optional<int> status;
    };
} // namespace javelin::jmap::api::detail

template <> struct glz::meta<javelin::jmap::api::detail::WebSocketMessageHeader>
{
    using T = javelin::jmap::api::detail::WebSocketMessageHeader;
    static constexpr auto value =
        glz::object("@type", &T::type, "requestId", &T::requestId, "title", &T::title, "detail",
                    &T::detail, "status", &T::status);
};

namespace javelin::jmap::api
{
    Q_LOGGING_CATEGORY(logJmapWebSocketTransport, "jmap.transport.websocket")

    namespace
    {
        constexpr auto connectTimeout = std::chrono::seconds{15};
        constexpr auto responseTimeout = std::chrono::seconds{60};
        constexpr auto cancellationPollInterval = std::chrono::milliseconds{250};
        constexpr auto fallbackRetryInterval = std::chrono::hours{6};

        using WebSocketMessageHeader = detail::WebSocketMessageHeader;

        struct BufferedWebSocketMessage
        {
            std::string type;
            std::string payload;
        };

        struct WebSocketAttemptResult
        {
            JmapMethodTransportResult result;
            bool requestDispatched = false;
            bool validWebSocketResponse = false;
        };

        [[nodiscard]] TransportError cancelledError(const std::string_view message)
        {
            return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = std::string{message},
                .httpStatus = std::nullopt,
            };
        }

        [[nodiscard]] TransportError networkError(const std::string_view message)
        {
            return TransportError{
                .code = TransportErrorCode::NetworkFailure,
                .message = std::string{message},
                .httpStatus = std::nullopt,
            };
        }

        [[nodiscard]] TransportError decodingError(const std::string_view message)
        {
            return TransportError{
                .code = TransportErrorCode::ResponseDecodingFailed,
                .message = std::string{message},
                .httpStatus = std::nullopt,
            };
        }

        [[nodiscard]] QString requestErrorMessage(const WebSocketMessageHeader& header)
        {
            QString message = QStringLiteral("The JMAP WebSocket server rejected the request.");
            if (header.title.has_value() && !header.title->empty())
            {
                message = QString::fromStdString(*header.title);
            }
            if (header.detail.has_value() && !header.detail->empty())
            {
                message += QStringLiteral(": ") + QString::fromStdString(*header.detail);
            }
            if (header.status.has_value())
            {
                message += QStringLiteral(" (status %1)").arg(*header.status);
            }
            return message;
        }

        class WebSocketJmapConnection final
        {
          public:
            WebSocketJmapConnection()
            {
                m_pingTimer.setInterval(std::chrono::seconds{30});
                QObject::connect(&m_pingTimer, &QTimer::timeout, &m_socket,
                                 [this]()
                                 {
                                     if (m_socket.state() == QAbstractSocket::ConnectedState)
                                     {
                                         m_socket.ping();
                                     }
                                 });
                QObject::connect(&m_socket, &QWebSocket::connected, &m_socket,
                                 [this]()
                                 {
                                     m_opening = false;
                                     m_pingTimer.start();
                                     qCInfo(logJmapWebSocketTransport) << "connected";
                                 });
                QObject::connect(&m_socket, &QWebSocket::disconnected, &m_socket,
                                 [this]()
                                 {
                                     m_opening = false;
                                     m_pingTimer.stop();
                                     ++m_disconnectGeneration;
                                     m_ignoredRequestIds.clear();
                                     qCInfo(logJmapWebSocketTransport) << "disconnected";
                                 });
                QObject::connect(&m_socket, &QWebSocket::errorOccurred, &m_socket,
                                 [this](QAbstractSocket::SocketError)
                                 {
                                     m_opening = false;
                                     qCWarning(logJmapWebSocketTransport).noquote()
                                         << "socket error" << m_socket.errorString();
                                 });
                QObject::connect(&m_socket, &QWebSocket::textMessageReceived, &m_socket,
                                 [this](const QString& message) { bufferMessage(message); });
            }

            ~WebSocketJmapConnection()
            {
                m_socket.abort();
            }

            [[nodiscard]] QCoro::Task<WebSocketAttemptResult>
            call(const std::string_view webSocketUrl, const JmapMethodRequest& request)
            {
                if (request.cancellation.isCancellationRequested())
                {
                    co_return WebSocketAttemptResult{
                        .result =
                            cancelledError("JMAP method call cancelled before WebSocket dispatch"),
                    };
                }

                const auto connected = co_await ensureConnected(webSocketUrl, request.accessToken,
                                                                request.cancellation);
                if (const auto* error = std::get_if<TransportError>(&connected))
                {
                    co_return WebSocketAttemptResult{.result = *error};
                }

                const std::string requestId = nextRequestId();
                const auto payload = serializeWebSocketRequestEnvelope(request.envelope, requestId);
                if (!payload.has_value())
                {
                    co_return WebSocketAttemptResult{
                        .result =
                            ProtocolError{
                                .code = ProtocolErrorCode::InvalidResponse,
                                .message = "Failed to serialize RFC 8887 JMAP request envelope",
                            },
                    };
                }

                const auto disconnectGeneration = m_disconnectGeneration;
                const auto invalidMessageGeneration = m_invalidMessageGeneration;
                const qint64 bytesQueued =
                    m_socket.sendTextMessage(QString::fromStdString(*payload));
                if (bytesQueued < 0)
                {
                    co_return WebSocketAttemptResult{
                        .result = networkError("Failed to queue JMAP WebSocket request"),
                    };
                }

                qCDebug(logJmapWebSocketTransport).noquote()
                    << "request dispatched" << QString::fromStdString(requestId) << "methods"
                    << request.envelope.methodCalls.size() << "bytes" << bytesQueued;
                QElapsedTimer elapsed;
                elapsed.start();
                while (
                    elapsed.elapsed() <
                    std::chrono::duration_cast<std::chrono::milliseconds>(responseTimeout).count())
                {
                    const auto response = m_responses.find(requestId);
                    if (response != m_responses.end())
                    {
                        auto buffered = std::move(response->second);
                        m_responses.erase(response);
                        m_ignoredRequestIds.erase(requestId);

                        qCInfo(logJmapWebSocketTransport).noquote()
                            << "response received" << QString::fromStdString(requestId) << "type"
                            << QString::fromStdString(buffered.type) << "bytes"
                            << buffered.payload.size() << "elapsed_ms" << elapsed.elapsed();

                        if (buffered.type == "RequestError")
                        {
                            WebSocketMessageHeader header;
                            auto json = buffered.payload;
                            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(header, json))
                            {
                                co_return WebSocketAttemptResult{
                                    .result = decodingError(
                                        "Failed to parse JMAP WebSocket request error"),
                                    .requestDispatched = true,
                                };
                            }
                            co_return WebSocketAttemptResult{
                                .result =
                                    ProtocolError{
                                        .code = ProtocolErrorCode::InvalidResponse,
                                        .message = requestErrorMessage(header).toStdString(),
                                    },
                                .requestDispatched = true,
                                .validWebSocketResponse = true,
                            };
                        }

                        const auto parsed = parseResponseEnvelope(buffered.payload);
                        if (!parsed.ok())
                        {
                            co_return WebSocketAttemptResult{
                                .result = decodingError(parsed.error.value_or(
                                    "Failed to parse JMAP WebSocket response envelope")),
                                .requestDispatched = true,
                            };
                        }
                        co_return WebSocketAttemptResult{
                            .result = std::move(*parsed.value),
                            .requestDispatched = true,
                            .validWebSocketResponse = true,
                        };
                    }

                    if (request.cancellation.isCancellationRequested())
                    {
                        m_ignoredRequestIds.insert(requestId);
                        co_return WebSocketAttemptResult{
                            .result = cancelledError(
                                "JMAP method call cancelled while awaiting WebSocket response"),
                            .requestDispatched = true,
                        };
                    }
                    if (m_socket.state() != QAbstractSocket::ConnectedState ||
                        m_disconnectGeneration != disconnectGeneration)
                    {
                        m_ignoredRequestIds.insert(requestId);
                        co_return WebSocketAttemptResult{
                            .result = networkError(
                                "JMAP WebSocket disconnected before the response arrived"),
                            .requestDispatched = true,
                        };
                    }
                    if (m_invalidMessageGeneration != invalidMessageGeneration)
                    {
                        m_ignoredRequestIds.insert(requestId);
                        co_return WebSocketAttemptResult{
                            .result = decodingError(
                                "JMAP WebSocket returned an invalid response message"),
                            .requestDispatched = true,
                        };
                    }

                    static_cast<void>(co_await qCoro(&m_socket, &QWebSocket::textMessageReceived,
                                                     cancellationPollInterval));
                }

                m_ignoredRequestIds.insert(requestId);
                co_return WebSocketAttemptResult{
                    .result = networkError("Timed out waiting for JMAP WebSocket response"),
                    .requestDispatched = true,
                };
            }

          private:
            [[nodiscard]] QCoro::Task<std::variant<std::monostate, TransportError>>
            ensureConnected(const std::string_view webSocketUrl, const std::string_view accessToken,
                            const CancellationToken& cancellation)
            {
                const QUrl url{QString::fromStdString(std::string{webSocketUrl})};
                if (!url.isValid() ||
                    url.scheme().compare(QStringLiteral("wss"), Qt::CaseInsensitive) != 0)
                {
                    co_return decodingError(
                        "The advertised JMAP WebSocket endpoint is not a valid wss URL");
                }

                const std::string nextUrl{webSocketUrl};
                const std::string nextToken{accessToken};
                if (m_url != nextUrl || m_accessToken != nextToken)
                {
                    m_socket.abort();
                    m_responses.clear();
                    m_ignoredRequestIds.clear();
                    m_url = nextUrl;
                    m_accessToken = nextToken;
                    m_opening = false;
                }

                if (m_socket.state() == QAbstractSocket::ConnectedState &&
                    m_socket.subprotocol() == QStringLiteral("jmap"))
                {
                    co_return std::monostate{};
                }

                if (!m_opening)
                {
                    QNetworkRequest handshake{url};
                    handshake.setRawHeader("Authorization",
                                           QByteArray{"Bearer "} +
                                               QByteArray::fromStdString(m_accessToken));
                    QWebSocketHandshakeOptions options;
                    options.setSubprotocols({QStringLiteral("jmap")});
                    m_opening = true;
                    m_socket.open(handshake, options);
                    qCInfo(logJmapWebSocketTransport).noquote() << "connecting" << url.toString();
                }

                QElapsedTimer elapsed;
                elapsed.start();
                const auto timeoutMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(connectTimeout).count();
                while (elapsed.elapsed() < timeoutMs)
                {
                    if (cancellation.isCancellationRequested())
                    {
                        co_return cancelledError(
                            "JMAP method call cancelled while connecting WebSocket");
                    }
                    if (m_socket.state() == QAbstractSocket::ConnectedState)
                    {
                        m_opening = false;
                        if (m_socket.subprotocol() != QStringLiteral("jmap"))
                        {
                            m_socket.abort();
                            co_return decodingError(
                                "Server did not negotiate the jmap WebSocket subprotocol");
                        }
                        co_return std::monostate{};
                    }
                    if (!m_opening && m_socket.state() == QAbstractSocket::UnconnectedState)
                    {
                        const auto error = m_socket.errorString();
                        co_return networkError(
                            error.isEmpty() ? std::string_view{"JMAP WebSocket handshake failed"}
                                            : std::string_view{error.toStdString()});
                    }

                    const auto remaining = std::chrono::milliseconds{
                        std::max<qint64>(1, timeoutMs - elapsed.elapsed())};
                    static_cast<void>(
                        co_await qCoro(&m_socket, &QWebSocket::stateChanged,
                                       std::min(remaining, cancellationPollInterval)));
                }

                m_opening = false;
                m_socket.abort();
                co_return networkError("Timed out establishing JMAP WebSocket");
            }

            void bufferMessage(const QString& message)
            {
                WebSocketMessageHeader header;
                auto json = message.toStdString();
                if (glz::read<glz::opts{.error_on_unknown_keys = false}>(header, json) ||
                    !header.type.has_value())
                {
                    ++m_invalidMessageGeneration;
                    qCWarning(logJmapWebSocketTransport) << "invalid WebSocket message";
                    return;
                }

                if (*header.type != "Response" && *header.type != "RequestError")
                {
                    qCDebug(logJmapWebSocketTransport).noquote()
                        << "ignoring unsolicited message type"
                        << QString::fromStdString(*header.type);
                    return;
                }
                if (!header.requestId.has_value() || header.requestId->empty())
                {
                    ++m_invalidMessageGeneration;
                    qCWarning(logJmapWebSocketTransport) << "response omitted required requestId";
                    return;
                }
                if (m_ignoredRequestIds.erase(*header.requestId) > 0)
                {
                    return;
                }

                m_responses.insert_or_assign(*header.requestId, BufferedWebSocketMessage{
                                                                    .type = *header.type,
                                                                    .payload = std::move(json),
                                                                });
            }

            [[nodiscard]] std::string nextRequestId()
            {
                ++m_nextRequestId;
                return "javelin-" + std::to_string(m_nextRequestId);
            }

            QWebSocket m_socket;
            QTimer m_pingTimer;
            std::string m_url;
            std::string m_accessToken;
            std::unordered_map<std::string, BufferedWebSocketMessage> m_responses;
            std::unordered_set<std::string> m_ignoredRequestIds;
            std::uint64_t m_nextRequestId = 0;
            std::uint64_t m_disconnectGeneration = 0;
            std::uint64_t m_invalidMessageGeneration = 0;
            bool m_opening = false;
        };
    } // namespace

    HttpJmapMethodTransport::HttpJmapMethodTransport(AbstractTransport& transport)
        : m_transport(transport)
    {
    }

    QCoro::Task<JmapMethodTransportResult> HttpJmapMethodTransport::call(JmapMethodRequest request)
    {
        if (request.cancellation.isCancellationRequested())
        {
            co_return TransportError{
                .code = TransportErrorCode::Cancelled,
                .message = "JMAP method call cancelled before HTTP dispatch",
                .httpStatus = std::nullopt,
            };
        }

        const auto body = serializeRequestEnvelope(request.envelope);
        if (!body.has_value())
        {
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = "Failed to serialize JMAP request envelope",
            };
        }

        const auto result = co_await m_transport.send(HttpRequest{
            .method = HttpMethod::Post,
            .url = QUrl{QString::fromStdString(request.apiUrl)},
            .headers =
                {
                    HttpHeader{.name = "Authorization",
                               .value = QByteArray{"Bearer "} +
                                        QByteArray::fromStdString(request.accessToken)},
                    HttpHeader{.name = "Accept", .value = "application/json"},
                    HttpHeader{.name = "Content-Type", .value = "application/json"},
                },
            .body = QByteArray::fromStdString(*body),
            .cancellation = std::move(request.cancellation),
        });
        if (const auto* error = std::get_if<TransportError>(&result))
        {
            co_return *error;
        }

        const auto parsed =
            parseResponseEnvelope(std::get<HttpResponse>(result).body.toStdString());
        if (!parsed.ok())
        {
            co_return ProtocolError{
                .code = ProtocolErrorCode::InvalidResponse,
                .message = *parsed.error,
            };
        }
        co_return *parsed.value;
    }

    struct PreferredJmapMethodTransport::Impl
    {
        javelin::jmap::cache::DatabaseConnection& databaseConnection;
        HttpJmapMethodTransport& httpTransport;
        std::unordered_map<std::string, std::unique_ptr<WebSocketJmapConnection>> connections;
    };

    PreferredJmapMethodTransport::PreferredJmapMethodTransport(
        javelin::jmap::cache::DatabaseConnection& databaseConnection,
        HttpJmapMethodTransport& httpTransport)
        : m_impl(std::make_unique<Impl>(Impl{
              .databaseConnection = databaseConnection,
              .httpTransport = httpTransport,
              .connections = {},
          }))
    {
    }

    PreferredJmapMethodTransport::~PreferredJmapMethodTransport() = default;

    QCoro::Task<JmapMethodTransportResult>
    PreferredJmapMethodTransport::call(JmapMethodRequest request)
    {
        if (request.accountId.empty())
        {
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }

        javelin::jmap::cache::JmapTransportPreferenceRepository preferences{
            m_impl->databaseConnection};
        const auto targetResult = preferences.resolve(request.accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&targetResult))
        {
            qCWarning(logJmapWebSocketTransport).noquote()
                << "could not resolve transport preference; using HTTP" << error->message;
            if (request.transportPolicy == JmapTransportPolicy::ForceWebSocket)
            {
                co_return networkError(
                    "WebSocket is required, but its endpoint could not be resolved from the cache");
            }
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }

        const auto& target =
            std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(targetResult);
        const bool forceWebSocket = request.transportPolicy == JmapTransportPolicy::ForceWebSocket;
        if (!target.has_value())
        {
            if (forceWebSocket)
            {
                qCWarning(logJmapWebSocketTransport)
                    << "forced WebSocket request has no advertised endpoint";
                co_return networkError("WebSocket is required, but this account has no advertised "
                                       "JMAP WebSocket endpoint");
            }
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }
        if (!forceWebSocket && !target->shouldAttemptWebSocket(QDateTime::currentDateTimeUtc()))
        {
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }

        qCDebug(logJmapWebSocketTransport).noquote()
            << "transport selected" << (forceWebSocket ? "forced" : "preferred") << "account"
            << QString::fromStdString(request.accountId) << "methods"
            << request.envelope.methodCalls.size();

        auto& connection = m_impl->connections[target->ownerAccountId];
        if (connection == nullptr)
        {
            connection = std::make_unique<WebSocketJmapConnection>();
        }

        auto attempt = co_await connection->call(target->webSocketUrl, request);
        if (attempt.validWebSocketResponse)
        {
            if (const auto error = preferences.markWebSocketAvailable(target->ownerAccountId,
                                                                      target->webSocketUrl))
            {
                qCWarning(logJmapWebSocketTransport).noquote()
                    << "could not persist working WebSocket state" << error->message;
            }
            co_return std::move(attempt.result);
        }

        const auto* transportError = std::get_if<TransportError>(&attempt.result);
        if (transportError == nullptr || transportError->code == TransportErrorCode::Cancelled)
        {
            co_return std::move(attempt.result);
        }

        const auto retryAfter = QDateTime::currentDateTimeUtc().addSecs(
            std::chrono::duration_cast<std::chrono::seconds>(fallbackRetryInterval).count());
        if (const auto error = preferences.markHttpFallback(
                target->ownerAccountId, target->webSocketUrl, retryAfter,
                QString::fromStdString(transportError->message)))
        {
            qCWarning(logJmapWebSocketTransport).noquote()
                << "could not persist HTTP fallback state" << error->message;
        }
        qCWarning(logJmapWebSocketTransport).noquote()
            << "WebSocket unavailable; remembering HTTP fallback until"
            << retryAfter.toString(Qt::ISODateWithMs)
            << QString::fromStdString(transportError->message);

        if (!attempt.requestDispatched)
        {
            if (forceWebSocket)
            {
                qCWarning(logJmapWebSocketTransport)
                    << "forced WebSocket request failed; HTTP fallback disabled";
                co_return std::move(attempt.result);
            }
            co_return co_await m_impl->httpTransport.call(std::move(request));
        }
        co_return std::move(attempt.result);
    }

} // namespace javelin::jmap::api
