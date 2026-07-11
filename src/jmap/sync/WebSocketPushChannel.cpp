#include "jmap/sync/WebSocketPushChannel.h"

#include "jmap/api/Error.h"

#include <QCoroSignal>
#include <QCoroTimer>

#include <QElapsedTimer>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <glaze/glaze.hpp>

#include <deque>
#include <unordered_map>

namespace javelin::jmap::sync
{
    struct StateChange
    {
        std::optional<std::string> type;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> changed;
        std::optional<std::string> pushState;
    };

    struct PushEnable
    {
        std::string type = "WebSocketPushEnable";
        std::vector<std::string> dataTypes{"Email", "Mailbox"};
        std::optional<std::string> pushState;
    };

    namespace
    {
        constexpr auto connectTimeout = std::chrono::seconds{15};
        constexpr auto messageTimeout = std::chrono::seconds{350};

    } // namespace
} // namespace javelin::jmap::sync

template <> struct glz::meta<javelin::jmap::sync::StateChange>
{
    using T = javelin::jmap::sync::StateChange;
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
    WebSocketPushChannel::WebSocketPushChannel(std::string url, std::string accessToken,
                                               LongPollStatusCallback statusCallback)
        : m_url(std::move(url)), m_accessToken(std::move(accessToken)),
          m_statusCallback(std::move(statusCallback))
    {
    }

    WebSocketPushChannel::~WebSocketPushChannel()
    {
        cancel();
    }

    void WebSocketPushChannel::cancel()
    {
        if (m_activeSocket != nullptr)
        {
            m_activeSocket->abort();
        }
    }

    QCoro::Task<LongPollResult> WebSocketPushChannel::poll(LongPollRequest request,
                                                           AbstractLongPollObserver& observer,
                                                           LongPollCancellation& cancellation)
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
        const auto connected = co_await qCoro(&socket, &QWebSocket::connected, connectTimeout);
        if (!connected.has_value())
        {
            socket.abort();
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                .message = "Timed out establishing JMAP WebSocket.",
                .httpStatus = std::nullopt,
            };
        }
        if (socket.subprotocol() != QStringLiteral("jmap"))
        {
            socket.close();
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                .message = "Server did not negotiate the jmap WebSocket subprotocol.",
                .httpStatus = std::nullopt,
            };
        }
        if (m_statusCallback)
        {
            m_statusCallback(LongPollConnectionStatus::Connected);
        }

        const PushEnable pushEnable{
            .type = "WebSocketPushEnable",
            .dataTypes = {"Email", "Mailbox"},
            .pushState =
                request.lastState.empty() ? std::nullopt : std::optional{request.lastState},
        };
        std::string enable;
        if (glz::write_json(pushEnable, enable))
        {
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::ResponseDecodingFailed,
                .message = "Failed to encode WebSocketPushEnable.",
                .httpStatus = std::nullopt,
            };
        }
        socket.sendTextMessage(QString::fromStdString(enable));

        QTimer pingTimer;
        pingTimer.setInterval(std::chrono::seconds{30});
        QObject::connect(&pingTimer, &QTimer::timeout, &socket, [&socket]() { socket.ping(); });
        pingTimer.start();

        std::deque<QString> messages;
        QElapsedTimer lastActivity;
        lastActivity.start();
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &socket,
                         [&messages, &lastActivity](QString message)
                         {
                             lastActivity.restart();
                             messages.push_back(std::move(message));
                         });
        QObject::connect(&socket, &QWebSocket::pong, &socket,
                         [&lastActivity](quint64, const QByteArray&) { lastActivity.restart(); });

        LongPollStreamSummary summary{.lastState = request.lastState, .updateCount = 0};
        while (!cancellation.isCancelled())
        {
            if (socket.state() != QAbstractSocket::ConnectedState)
            {
                co_return javelin::jmap::api::TransportError{
                    .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
                    .message = "JMAP WebSocket disconnected.",
                    .httpStatus = std::nullopt,
                };
            }
            if (lastActivity.elapsed() >
                std::chrono::duration_cast<std::chrono::milliseconds>(messageTimeout).count())
            {
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

            StateChange change;
            std::string buffer = messages.front().toStdString();
            messages.pop_front();
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(change, buffer) ||
                change.type != std::optional<std::string>{"StateChange"})
            {
                continue;
            }
            const auto account = change.changed.find(request.accountId);
            if (account == change.changed.end())
            {
                continue;
            }
            LongPollResponse response;
            response.newState = change.pushState.value_or(summary.lastState);
            for (const auto& [type, state] : account->second)
            {
                static_cast<void>(state);
                response.changedTypes.push_back(type);
            }
            summary.lastState = response.newState;
            ++summary.updateCount;
            co_await observer.onUpdate(std::move(response));
        }
        socket.close();
        co_return summary;
    }
} // namespace javelin::jmap::sync
