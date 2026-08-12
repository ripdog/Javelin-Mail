#include "protocol/LocalDaemonServer.h"
#include "protocol/SocketWireCodecInternal.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace javelin::protocol
{
    using namespace detail;

    namespace
    {
        [[nodiscard]] SocketFrameError malformed(QString detail)
        {
            return {.code = SocketFrameErrorCode::MalformedPayload, .detail = std::move(detail)};
        }
    } // namespace
    SocketDaemonEndpoint::SocketDaemonEndpoint(DaemonRequestHandler& handler,
                                               SocketEndpointOptions options, QObject* parent)
        : QObject(parent), m_handler(handler), m_options(std::move(options)),
          m_decoder(m_options.limits)
    {
    }

    SocketDaemonEndpoint::~SocketDaemonEndpoint()
    {
        close();
    }

    std::optional<SocketTransportError> SocketDaemonEndpoint::listen()
    {
        if (m_server == nullptr)
        {
            m_server = std::make_unique<QLocalServer>();
            m_server->setSocketOptions(QLocalServer::UserAccessOption);
            connect(m_server.get(), &QLocalServer::newConnection, this,
                    &SocketDaemonEndpoint::acceptConnection);
        }
        m_lastError = listenOnLocalServer(*m_server, m_options,
                                          QStringLiteral("socket path is already in use"));
        return m_lastError;
    }

    void SocketDaemonEndpoint::close()
    {
        if (m_socket != nullptr)
            disconnect(SocketDisconnectReason::ServerShutdown,
                       QStringLiteral("socket endpoint is shutting down"));
        closeLocalServer(m_server.get(), m_options.socketPath);
    }

    std::optional<SocketTransportError> SocketDaemonEndpoint::lastError() const
    {
        return m_lastError;
    }

    void SocketDaemonEndpoint::publishEvent(const BoundaryEvent& event)
    {
        if (m_socket == nullptr || !m_handshakeComplete)
            return;
        if (!enqueueEvent(event))
            disconnect(SocketDisconnectReason::QueueOverflow,
                       QStringLiteral("socket output queue cannot carry the boundary event"));
    }

    void SocketDaemonEndpoint::acceptConnection()
    {
        while (m_server != nullptr && m_server->hasPendingConnections())
        {
            auto* candidate = m_server->nextPendingConnection();
            if (candidate == nullptr)
                continue;
            if (m_socket != nullptr)
            {
                candidate->disconnectFromServer();
                candidate->deleteLater();
                continue;
            }
            if (const auto error =
                    validatePeerCredentials(*candidate, m_options.enforcePeerCredentials))
            {
                candidate->disconnectFromServer();
                candidate->deleteLater();
                m_lastError = error;
                continue;
            }
            m_socket.reset(candidate);
            m_socket->setReadBufferSize(static_cast<qint64>(m_options.limits.maximumFrameBytes));
            connect(m_socket.get(), &QLocalSocket::readyRead, this,
                    &SocketDaemonEndpoint::readSocket);
            connect(m_socket.get(), &QLocalSocket::bytesWritten, this,
                    &SocketDaemonEndpoint::writeSocket);
            connect(m_socket.get(), &QLocalSocket::disconnected, this,
                    &SocketDaemonEndpoint::socketDisconnected);
            connect(m_socket.get(), &QLocalSocket::errorOccurred, this,
                    &SocketDaemonEndpoint::socketError);
            m_decoder.clear();
            m_handshakeComplete = false;
            m_lastError.reset();
            Q_EMIT connectionOpened();
        }
    }

    void SocketDaemonEndpoint::readSocket()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket == nullptr)
            return;
        if (const auto error = m_decoder.append(m_socket->readAll()))
        {
            sendProtocolError(0, *error);
            return;
        }
        while (m_socket != nullptr && !m_processingFrame)
        {
            auto decoded = m_decoder.takeFrame();
            if (auto* error = std::get_if<SocketFrameError>(&decoded))
            {
                sendProtocolError(0, *error);
                return;
            }
            auto& frame = std::get<std::optional<SocketFrame>>(decoded);
            if (!frame.has_value())
                return;
            m_processingFrame = true;
            handleFrame(*frame);
            m_processingFrame = false;
        }
    }

    void SocketDaemonEndpoint::writeSocket(const qint64)
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        pumpWrites();
    }

    void SocketDaemonEndpoint::socketDisconnected()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            disconnect(SocketDisconnectReason::PeerClosed,
                       QStringLiteral("socket peer disconnected"));
    }

    void SocketDaemonEndpoint::socketError()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            disconnect(SocketDisconnectReason::TransportFailure, m_socket->errorString());
    }

    void SocketDaemonEndpoint::handleFrame(const SocketFrame& frame)
    {
        const auto rawKind = static_cast<std::uint16_t>(frame.kind);
        if (frame.correlation == 0)
        {
            sendProtocolError(0, malformed(QStringLiteral("request correlation is zero")));
            return;
        }
        if (frame.kind == SocketFrameKind::ProtocolError ||
            rawKind >= static_cast<std::uint16_t>(SocketFrameKind::HelloReply))
        {
            sendProtocolError(frame.correlation,
                              malformed(QStringLiteral("unexpected socket message kind")));
            return;
        }
        if (frame.kind == SocketFrameKind::ReadyForActivationRequest)
        {
            if (!m_handshakeComplete || !frame.payload.isEmpty())
            {
                sendProtocolError(frame.correlation,
                                  malformed(QStringLiteral("invalid activation request frame")));
                return;
            }
            const auto reply =
                encodeOptionalError(SocketFrameKind::ReadyForActivationReply,
                                    m_handler.handleGuiReadyForActivation(), m_options.limits);
            if (auto* error = std::get_if<SocketFrameError>(&reply))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            const auto frameData = encodeSocketFrame(
                SocketFrameKind::ReadyForActivationReply, frame.correlation,
                std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                      .kind = SocketFrameKind::ReadyForActivationReply,
                                      .correlation = frame.correlation,
                                      .event = std::nullopt}))
                disconnect(SocketDisconnectReason::QueueOverflow,
                           QStringLiteral("socket output queue is full"));
            return;
        }

        auto decoded = decodeClientRequest(frame.kind, frame.payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&decoded))
        {
            sendProtocolError(frame.correlation, *error);
            return;
        }
        auto request = std::move(std::get<ClientRequest>(decoded));
        if (const auto error = validate(request, m_options.limits))
        {
            if (const auto* command = std::get_if<CommandRequest>(&request))
            {
                const auto reply = encodeCommandReply(
                    CommandRejected{.id = command->id, .error = *error}, m_options.limits);
                if (auto* encodedError = std::get_if<SocketFrameError>(&reply))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      encodedError->detail);
                const auto frameData = encodeSocketFrame(
                    SocketFrameKind::CommandReplyFrame, frame.correlation,
                    std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
                if (auto* frameError = std::get_if<SocketFrameError>(&frameData))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      frameError->detail);
                if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                          .kind = SocketFrameKind::CommandReplyFrame,
                                          .correlation = frame.correlation,
                                          .event = std::nullopt}))
                    disconnect(SocketDisconnectReason::QueueOverflow,
                               QStringLiteral("socket output queue is full"));
                return;
            }
            if (const auto* materialization = std::get_if<MaterializationRequest>(&request))
            {
                const auto reply = encodeMaterializationReply(
                    MaterializationRejected{.id = materialization->id, .error = *error},
                    m_options.limits);
                if (auto* encodedError = std::get_if<SocketFrameError>(&reply))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      encodedError->detail);
                const auto frameData = encodeSocketFrame(
                    SocketFrameKind::MaterializationReplyFrame, frame.correlation,
                    std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
                if (auto* frameError = std::get_if<SocketFrameError>(&frameData))
                    return disconnect(SocketDisconnectReason::ProtocolViolation,
                                      frameError->detail);
                if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                          .kind = SocketFrameKind::MaterializationReplyFrame,
                                          .correlation = frame.correlation,
                                          .event = std::nullopt}))
                    disconnect(SocketDisconnectReason::QueueOverflow,
                               QStringLiteral("socket output queue is full"));
                return;
            }
            sendProtocolError(frame.correlation,
                              malformed(QStringLiteral("request validation failed")));
            return;
        }

        if (const auto* hello = std::get_if<HelloRequest>(&request))
        {
            if (m_handshakeComplete)
            {
                sendProtocolError(frame.correlation,
                                  malformed(QStringLiteral("HELLO was sent twice")));
                return;
            }
            if (hello->protocol.major != m_options.protocol.major ||
                hello->protocol.minor > m_options.protocol.minor ||
                (m_options.expectedBuild.has_value() && hello->build != *m_options.expectedBuild))
            {
                HandshakeRejected rejected{
                    .error = {.code = hello->protocol.major != m_options.protocol.major
                                          ? BoundaryErrorCode::InvalidProtocol
                                          : BoundaryErrorCode::IncompatibleBuild,
                              .field = QStringLiteral("hello"),
                              .detail = QStringLiteral(
                                  "socket peer is not compatible with this daemon")}};
                const auto reply = encodeHandshakeReply(rejected, m_options.limits);
                if (auto* error = std::get_if<SocketFrameError>(&reply))
                    return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                const auto frameData = encodeSocketFrame(
                    SocketFrameKind::HelloReply, frame.correlation,
                    std::get<EncodedPayload>(reply).payload, m_options.limits.maximumFrameBytes);
                if (auto* error = std::get_if<SocketFrameError>(&frameData))
                    return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                          .kind = SocketFrameKind::HelloReply,
                                          .correlation = frame.correlation,
                                          .event = std::nullopt}))
                    disconnect(SocketDisconnectReason::QueueOverflow,
                               QStringLiteral("socket output queue is full"));
                return;
            }
            const auto reply = m_handler.handleHello(*hello);
            const auto encoded = encodeHandshakeReply(reply, m_options.limits);
            if (auto* error = std::get_if<SocketFrameError>(&encoded))
                return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
            const auto frameData = encodeSocketFrame(SocketFrameKind::HelloReply, frame.correlation,
                                                     std::get<EncodedPayload>(encoded).payload,
                                                     m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
                return disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
            if (isReadyReply(reply))
                m_handshakeComplete = true;
            else
            {
                // A rejected handshake is returned before the connection is retired.
                m_closeAfterWrites = true;
                m_closeReason = SocketDisconnectReason::IncompatiblePeer;
                m_closeDetail = QStringLiteral("daemon rejected the socket handshake");
            }
            if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                      .kind = SocketFrameKind::HelloReply,
                                      .correlation = frame.correlation,
                                      .event = std::nullopt}))
                disconnect(SocketDisconnectReason::QueueOverflow,
                           QStringLiteral("socket output queue is full"));
            return;
        }

        if (!m_handshakeComplete)
        {
            sendProtocolError(frame.correlation,
                              malformed(QStringLiteral("HELLO is required before requests")));
            return;
        }

        const auto enqueueReply =
            [this, &frame](const SocketFrameKind kind, const EncodedPayloadResult& encoded)
        {
            if (auto* error = std::get_if<SocketFrameError>(&encoded))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            const auto frameData = encodeSocketFrame(kind, frame.correlation,
                                                     std::get<EncodedPayload>(encoded).payload,
                                                     m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
            {
                disconnect(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                      .kind = kind,
                                      .correlation = frame.correlation,
                                      .event = std::nullopt}))
                disconnect(SocketDisconnectReason::QueueOverflow,
                           QStringLiteral("socket output queue is full"));
        };

        std::visit(
            [this, &enqueueReply](auto&& value)
            {
                using Request = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Request, CommandRequest>)
                    enqueueReply(SocketFrameKind::CommandReplyFrame,
                                 encodeCommandReply(m_handler.handleCommand(std::move(value)),
                                                    m_options.limits));
                else if constexpr (std::is_same_v<Request, MaterializationRequest>)
                {
                    m_activeScopes.push_back(value.scope);
                    enqueueReply(
                        SocketFrameKind::MaterializationReplyFrame,
                        encodeMaterializationReply(
                            m_handler.handleMaterialization(std::move(value)), m_options.limits));
                }
                else if constexpr (std::is_same_v<Request, CancelMaterializationScopeRequest>)
                {
                    m_handler.handleCancelMaterializationScope(value);
                    std::erase(m_activeScopes, value.scope);
                    enqueueReply(
                        SocketFrameKind::CancelMaterializationScopeReply,
                        encodeOptionalError(SocketFrameKind::CancelMaterializationScopeReply,
                                            std::nullopt, m_options.limits));
                }
                else if constexpr (std::is_same_v<Request, GetSettingsRequest>)
                    enqueueReply(SocketFrameKind::SettingsReadReplyFrame,
                                 encodeSettingsReadReply(m_handler.handleGetSettings(value),
                                                         m_options.limits));
                else if constexpr (std::is_same_v<Request, UpdateSettingsRequest>)
                    enqueueReply(
                        SocketFrameKind::SettingsUpdateReplyFrame,
                        encodeSettingsUpdateReply(m_handler.handleUpdateSettings(std::move(value)),
                                                  m_options.limits));
                else if constexpr (std::is_same_v<Request, CacheAccessSuspendedAcknowledgement>)
                    enqueueReply(SocketFrameKind::CacheAccessSuspendedReply,
                                 encodeOptionalError(SocketFrameKind::CacheAccessSuspendedReply,
                                                     m_handler.handleCacheAccessSuspended(value),
                                                     m_options.limits));
                else if constexpr (std::is_same_v<Request, PingRequest>)
                    enqueueReply(SocketFrameKind::PingReply,
                                 encodeOptionalError(SocketFrameKind::PingReply,
                                                     m_handler.handlePing(value),
                                                     m_options.limits));
            },
            std::move(request));
    }

    void SocketDaemonEndpoint::sendProtocolError(const std::uint64_t correlation,
                                                 const SocketFrameError& error)
    {
        const auto payload = encodeProtocolError(error, m_options.limits);
        if (std::holds_alternative<SocketFrameError>(payload))
            return;
        const auto frameData = encodeSocketFrame(SocketFrameKind::ProtocolError, correlation,
                                                 std::get<EncodedPayload>(payload).payload,
                                                 m_options.limits.maximumFrameBytes);
        if (std::holds_alternative<SocketFrameError>(frameData))
            return;
        m_closeAfterWrites = true;
        m_closeReason = SocketDisconnectReason::ProtocolViolation;
        m_closeDetail = error.detail;
        if (!enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                  .kind = SocketFrameKind::ProtocolError,
                                  .correlation = correlation,
                                  .event = std::nullopt}))
            disconnect(SocketDisconnectReason::QueueOverflow,
                       QStringLiteral("socket output queue is full"));
    }

    bool SocketDaemonEndpoint::enqueue(PendingFrame frame)
    {
        const auto frameBytes = static_cast<std::size_t>(frame.data.size());
        if (m_pendingWrites.size() + (m_currentWrite != nullptr ? 1U : 0U) >=
                m_options.maximumQueuedFrames ||
            m_queuedBytes + frameBytes > m_options.maximumQueuedBytes)
            return false;
        m_queuedBytes += frameBytes;
        m_pendingWrites.push_back(std::move(frame));
        pumpWrites();
        return true;
    }

    bool SocketDaemonEndpoint::enqueueEvent(const BoundaryEvent& event)
    {
        const auto encoded = encodeBoundaryEvent(event, m_options.limits);
        if (std::holds_alternative<SocketFrameError>(encoded))
            return false;
        const auto frameData = encodeSocketFrame(SocketFrameKind::BoundaryEventFrame, 0,
                                                 std::get<EncodedPayload>(encoded).payload,
                                                 m_options.limits.maximumFrameBytes);
        if (std::holds_alternative<SocketFrameError>(frameData))
            return false;
        const auto coalescible = std::holds_alternative<CacheInvalidation>(event) ||
                                 std::holds_alternative<DaemonStatusChanged>(event) ||
                                 std::holds_alternative<SettingsUpdated>(event);
        if (coalescible)
        {
            for (auto iterator = m_pendingWrites.rbegin(); iterator != m_pendingWrites.rend();
                 ++iterator)
            {
                if (!sameEventKind(iterator->event, std::optional<BoundaryEvent>{event}))
                    continue;
                if (auto* target = std::get_if<CacheInvalidation>(&*iterator->event))
                {
                    const auto& source = std::get<CacheInvalidation>(event);
                    if (target->accountId != source.accountId)
                        continue;
                    mergeInvalidation(*target, source, m_options.limits);
                    const auto merged = encodeBoundaryEvent(*iterator->event, m_options.limits);
                    if (std::holds_alternative<SocketFrameError>(merged))
                        return false;
                    const auto mergedFrame =
                        encodeSocketFrame(SocketFrameKind::BoundaryEventFrame, 0,
                                          std::get<EncodedPayload>(merged).payload,
                                          m_options.limits.maximumFrameBytes);
                    if (std::holds_alternative<SocketFrameError>(mergedFrame))
                        return false;
                    m_queuedBytes -= static_cast<std::size_t>(iterator->data.size());
                    iterator->data = std::get<QByteArray>(mergedFrame);
                    m_queuedBytes += static_cast<std::size_t>(iterator->data.size());
                }
                else
                {
                    m_queuedBytes -= static_cast<std::size_t>(iterator->data.size());
                    iterator->data = std::get<QByteArray>(frameData);
                    iterator->event = event;
                    m_queuedBytes += static_cast<std::size_t>(iterator->data.size());
                }
                return true;
            }
        }
        PendingFrame frame{.data = std::get<QByteArray>(frameData),
                           .kind = SocketFrameKind::BoundaryEventFrame,
                           .correlation = 0,
                           .coalescible = coalescible,
                           .event = event};
        const bool queueFull = m_pendingWrites.size() + (m_currentWrite != nullptr ? 1U : 0U) >=
                                   m_options.maximumQueuedFrames ||
                               m_queuedBytes + static_cast<std::size_t>(frame.data.size()) >
                                   m_options.maximumQueuedBytes;
        if (queueFull && (std::holds_alternative<DaemonStatusChanged>(event) ||
                          std::holds_alternative<SettingsUpdated>(event) ||
                          std::holds_alternative<DaemonLogEntries>(event)))
            return true;
        return enqueue(std::move(frame));
    }

    void SocketDaemonEndpoint::pumpWrites()
    {
        if (m_socket == nullptr)
            return;
        if (m_currentWrite != nullptr)
        {
            auto& completed = *m_currentWrite;
            if (completed.offset != static_cast<std::size_t>(completed.data.size()) ||
                m_socket->bytesToWrite() != 0)
                return;
            m_queuedBytes -= static_cast<std::size_t>(completed.data.size());
            m_currentWrite.reset();
        }
        if (m_pendingWrites.empty())
        {
            if (m_closeAfterWrites)
                disconnect(m_closeReason, m_closeDetail);
            return;
        }
        m_currentWrite = std::make_unique<PendingFrame>(std::move(m_pendingWrites.front()));
        m_pendingWrites.pop_front();
        auto& frame = *m_currentWrite;
        while (frame.offset < static_cast<std::size_t>(frame.data.size()))
        {
            const auto remaining = frame.data.size() - static_cast<qsizetype>(frame.offset);
            const auto written = m_socket->write(frame.data.constData() + frame.offset, remaining);
            if (written < 0)
            {
                disconnect(SocketDisconnectReason::TransportFailure, m_socket->errorString());
                return;
            }
            if (written == 0)
                break;
            frame.offset += static_cast<std::size_t>(written);
        }
        if (frame.offset == static_cast<std::size_t>(frame.data.size()) &&
            m_socket->bytesToWrite() == 0)
        {
            m_queuedBytes -= static_cast<std::size_t>(frame.data.size());
            m_currentWrite.reset();
            pumpWrites();
        }
    }

    void SocketDaemonEndpoint::disconnect(const SocketDisconnectReason reason, QString detail)
    {
        if (m_socket == nullptr)
            return;
        if (m_inSocketCallback)
        {
            if (m_disconnectScheduled)
                return;
            m_disconnectScheduled = true;
            QMetaObject::invokeMethod(
                this,
                [this, reason, detail = std::move(detail)]() mutable
                {
                    m_disconnectScheduled = false;
                    disconnect(reason, std::move(detail));
                },
                Qt::QueuedConnection);
            return;
        }
        for (const auto& scope : std::exchange(m_activeScopes, {}))
            m_handler.handleCancelMaterializationScope({.scope = scope});
        if (m_socket != nullptr)
        {
            QSignalBlocker blocker{m_socket.get()};
            m_socket->abort();
        }
        clearSocket();
        m_lastError = SocketTransportError{.reason = reason, .detail = std::move(detail)};
        Q_EMIT connectionClosed(reason, m_lastError->detail);
    }

    void SocketDaemonEndpoint::clearSocket()
    {
        m_socket.reset();
        m_decoder.clear();
        m_pendingWrites.clear();
        m_currentWrite.reset();
        m_queuedBytes = 0;
        m_handshakeComplete = false;
        m_disconnectScheduled = false;
        m_closeAfterWrites = false;
        m_closeReason = SocketDisconnectReason::None;
        m_closeDetail.clear();
    }

} // namespace javelin::protocol
