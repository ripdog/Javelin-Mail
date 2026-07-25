#include "jmap/sync/WebSocketPushChannel.h"

#include "jmap/api/Error.h"

#include <QCoroSignal>
#include <QCoroTimer>

#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QStringList>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <glaze/glaze.hpp>

#include <deque>
#include <unordered_map>

namespace javelin::jmap::sync
{
    Q_LOGGING_CATEGORY(logWebSocket, "jmap.push.websocket")
    struct WebSocketStateChange
    {
        std::optional<std::string> type;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> changed;
        std::optional<std::string> pushState;
    };

    struct PushEnable
    {
        std::string type = "WebSocketPushEnable";
        std::vector<std::string> dataTypes;
        std::optional<std::string> pushState;
    };

    namespace
    {
        constexpr auto connectTimeout = std::chrono::seconds{15};
        constexpr auto messageTimeout = std::chrono::seconds{350};

    } // namespace
} // namespace javelin::jmap::sync

template <> struct glz::meta<javelin::jmap::sync::WebSocketStateChange>
{
    using T = javelin::jmap::sync::WebSocketStateChange;
    static constexpr auto value =
        glz::object("@type", &T::type, "changed", &T::changed, "pushState", &T::pushState);
};

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

    void WebSocketStateChangeSource::reportConnectedActivity() const
    {
        if (m_statusCallback)
        {
            m_statusCallback(StateChangeConnectionStatus::Connected);
        }
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
        QWebSocket socket;
        m_activeSocket = &socket;
        const auto clearSocket = qScopeGuard([this]() { m_activeSocket = nullptr; });

        QNetworkRequest handshake{QUrl{QString::fromStdString(m_url)}};
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
        reportConnectedActivity();
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
        QElapsedTimer lastActivity;
        lastActivity.start();
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &socket,
                         [this, &messages, &lastActivity](QString message)
                         {
                             lastActivity.restart();
                             reportConnectedActivity();
                             messages.push_back(std::move(message));
                         });
        QObject::connect(&socket, &QWebSocket::pong, &socket,
                         [this, &lastActivity](quint64, const QByteArray&)
                         {
                             lastActivity.restart();
                             reportConnectedActivity();
                         });

        // Install the receive handlers before enabling push. A server may send the initial
        // StateChange immediately in response to WebSocketPushEnable.
        socket.sendTextMessage(QString::fromStdString(*enable));
        QStringList subscribedTypes;
        for (const auto& type : subscription.types)
            subscribedTypes.push_back(QString::fromStdString(type));
        qCDebug(logWebSocket).noquote()
            << "push subscription sent for" << subscribedTypes.join(QStringLiteral(", "));

        QTimer pingTimer;
        pingTimer.setInterval(std::chrono::seconds{30});
        QObject::connect(&pingTimer, &QTimer::timeout, &socket, [&socket]() { socket.ping(); });
        pingTimer.start();

        StateChangeStreamSummary summary{.lastState = subscription.lastState, .updateCount = 0};
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
            if (lastActivity.elapsed() >
                std::chrono::duration_cast<std::chrono::milliseconds>(messageTimeout).count())
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

            WebSocketStateChange change;
            std::string buffer = messages.front().toStdString();
            messages.pop_front();
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(change, buffer) ||
                change.type != std::optional<std::string>{"StateChange"})
            {
                qCWarning(logWebSocket) << "invalid push message received";
                continue;
            }
            const auto account = change.changed.find(subscription.accountId);
            if (account == change.changed.end())
            {
                continue;
            }
            StateChangeEvent event;
            event.newState = change.pushState.value_or(summary.lastState);
            for (const auto& [type, state] : account->second)
            {
                event.changedTypes.push_back(type);
                event.changedStates.emplace(type, state);
            }
            QStringList changedTypes;
            for (const auto& type : event.changedTypes)
                changedTypes.push_back(QString::fromStdString(type));
            qCInfo(logWebSocket).noquote() << "state change" << changedTypes.join(QLatin1Char(','));
            summary.lastState = event.newState;
            ++summary.updateCount;
            co_await consumer.onStateChange(std::move(event));
        }
        socket.close();
        qCInfo(logWebSocket) << "closed";
        co_return summary;
    }
} // namespace javelin::jmap::sync
