#include "jmap/sync/WebSocketPushChannel.h"

#include "jmap/api/Error.h"
#include "jmap/sync/PushActivityTracker.h"
#include "jmap/sync/PushProtocol.h"
#include "jmap/sync/PushStreamSession.h"

#include <QCoroSignal>
#include <QCoroTimer>

#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <glaze/glaze.hpp>

#include <deque>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logWebSocket, "jmap.push.ws")

    struct PushEnable
    {
        std::string type = "WebSocketPushEnable";
        std::vector<std::string> dataTypes;
        std::optional<std::string> pushState;
    };

    namespace
    {
        constexpr auto connectTimeout = std::chrono::seconds{15};

    } // namespace
} // namespace javelin::jmap::sync

template <> struct glz::meta<javelin::jmap::sync::PushEnable>
{
    using T = javelin::jmap::sync::PushEnable;
    static constexpr auto value =
        glz::object("@type", &T::type, "dataTypes", &T::dataTypes, "pushState", &T::pushState);
};

namespace javelin::jmap::sync
{
    std::optional<std::string>
    encodeWebSocketPushEnable(const StateChangeSubscription& subscription)
    {
        const PushEnable pushEnable{
            .type = "WebSocketPushEnable",
            .dataTypes = subscription.types,
            .pushState = subscription.lastState.empty() ? std::nullopt
                                                        : std::optional{subscription.lastState},
        };
        std::string encoded;
        if (glz::write_json(pushEnable, encoded))
            return std::nullopt;
        return encoded;
    }

    WebSocketStateChangeSource::WebSocketStateChangeSource(std::string url, std::string accessToken,
                                                           StateChangeStatusCallback statusCallback)
        : m_url(std::move(url)), m_accessToken(std::move(accessToken)),
          m_statusCallback(std::move(statusCallback))
    {
    }

    WebSocketStateChangeSource::~WebSocketStateChangeSource()
    {
        cancel();
    }

    void WebSocketStateChangeSource::cancel()
    {
        if (m_activeSocket != nullptr)
        {
            m_activeSocket->abort();
        }
    }

    QCoro::Task<StateChangeSourceResult>
    WebSocketStateChangeSource::consume(StateChangeSubscription subscription,
                                        StateChangeConsumer& consumer,
                                        StateChangeCancellation& cancellation)
    {
        const QUrl endpoint{QString::fromStdString(m_url)};
        PushActivityTracker activity{m_statusCallback, endpoint,
                                     pushActivityTimeout(requestedPushPingInterval)};
        QWebSocket socket;
        m_activeSocket = &socket;
        const auto clearSocket = qScopeGuard([this]() { m_activeSocket = nullptr; });

        QNetworkRequest handshake{endpoint};
        handshake.setRawHeader("Authorization",
                               QByteArray{"Bearer "} + QByteArray::fromStdString(m_accessToken));
        QWebSocketHandshakeOptions handshakeOptions;
        handshakeOptions.setSubprotocols({QStringLiteral("jmap")});
        socket.open(handshake, handshakeOptions);
        qCInfo(logWebSocket) << "connecting";
        const auto connected = co_await qCoro(&socket, &QWebSocket::connected, connectTimeout);
        if (!connected.has_value())
        {
            qCWarning(logWebSocket) << "connection timed out";
            socket.abort();
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                .message = "Timed out establishing JMAP WebSocket.",
                .httpStatus = std::nullopt,
            };
        }
        if (socket.subprotocol() != QStringLiteral("jmap"))
        {
            qCWarning(logWebSocket) << "server rejected jmap subprotocol";
            socket.close();
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                .message = "Server did not negotiate the jmap WebSocket subprotocol.",
                .httpStatus = std::nullopt,
            };
        }
        activity.recordActivity();
        qCInfo(logWebSocket) << "connected";

        const auto enable = encodeWebSocketPushEnable(subscription);
        if (!enable)
        {
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                .message = "Failed to encode WebSocketPushEnable.",
                .httpStatus = std::nullopt,
            };
        }
        std::deque<QString> messages;
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &socket,
                         [&messages, &activity](QString message)
                         {
                             activity.recordActivity();
                             messages.push_back(std::move(message));
                         });
        QObject::connect(&socket, &QWebSocket::pong, &socket,
                         [&activity](quint64, const QByteArray&)
                         {
                             activity.recordActivity();
                             qCDebug(logWebSocket).noquote()
                                 << "server ping interval" << requestedPushPingInterval.count()
                                 << "seconds" << activity.serverBaseUrl();
                         });

        // Install the receive handlers before enabling push. A server may send the initial
        // StateChange immediately in response to WebSocketPushEnable.
        socket.sendTextMessage(QString::fromStdString(*enable));
        QStringList subscribedTypes;
        for (const auto& type : subscription.types)
            subscribedTypes.push_back(QString::fromStdString(type));
        qCDebug(logWebSocket).noquote()
            << "push subscription sent for" << subscribedTypes.join(QStringLiteral(", "))
            << activity.serverBaseUrl();

        QTimer pingTimer;
        pingTimer.setInterval(requestedPushPingInterval);
        QObject::connect(&pingTimer, &QTimer::timeout, &socket, [&socket]() { socket.ping(); });
        pingTimer.start();

        PushStreamSession stream{std::move(subscription), consumer};
        while (!cancellation.isCancelled())
        {
            if (socket.state() != QAbstractSocket::ConnectedState)
            {
                qCWarning(logWebSocket) << "disconnected" << socket.errorString();
                co_return javelin::jmap::api::TransportError{
                    .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                    .message = "JMAP WebSocket disconnected.",
                    .httpStatus = std::nullopt,
                };
            }
            if (activity.hasTimedOut())
            {
                qCWarning(logWebSocket) << "activity timeout";
                socket.abort();
                co_return javelin::jmap::api::TransportError{
                    .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                    .message = "Timed out waiting for JMAP WebSocket activity.",
                    .httpStatus = std::nullopt,
                };
            }
            if (messages.empty())
            {
                QTimer waitTimer;
                waitTimer.setSingleShot(true);
                waitTimer.start(std::chrono::milliseconds{100});
                co_await qCoro(waitTimer).waitForTimeout();
                continue;
            }

            std::string buffer = messages.front().toStdString();
            messages.pop_front();
            auto outcome = co_await stream.accept(parseWebSocketPushMessage(
                stream.subscription(), stream.summary().lastState, buffer));
            if (const auto* error = std::get_if<PushStreamProtocolFailure>(&outcome))
            {
                qCWarning(logWebSocket).noquote()
                    << "invalid push message received" << QString::fromStdString(error->message);
            }
        }
        socket.close();
        qCInfo(logWebSocket) << "closed";
        co_return stream.summary();
    }
} // namespace javelin::jmap::sync
