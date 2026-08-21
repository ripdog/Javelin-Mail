#include "protocol/LocalDaemonClient.h"
#include "protocol/SocketWireCodecInternal.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopedValueRollback>
#include <QSignalBlocker>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace javelin::protocol
{
    using namespace detail;

    namespace
    {
        [[nodiscard]] bool isResumableBootstrapRequest(const SocketFrameKind kind)
        {
            return kind == SocketFrameKind::HelloRequest ||
                   kind == SocketFrameKind::GetSettingsRequest ||
                   kind == SocketFrameKind::ReadyForActivationRequest;
        }
    } // namespace
    SocketDaemonClient::SocketDaemonClient(SocketClientOptions options, QObject* parent)
        : QObject(parent), m_options(std::move(options)), m_decoder(m_options.limits)
    {
    }

    SocketDaemonClient::~SocketDaemonClient()
    {
        disconnectFromDaemon();
    }

    std::optional<SocketTransportError> SocketDaemonClient::connectToDaemon()
    {
        if (isConnected())
            return std::nullopt;
        if (const auto error = validateRuntimeDirectory(m_options))
        {
            m_lastError = error;
            return error;
        }
        m_socket = std::make_unique<QLocalSocket>();
        m_socket->setReadBufferSize(static_cast<qint64>(m_options.limits.maximumFrameBytes));
        connect(m_socket.get(), &QLocalSocket::readyRead, this, &SocketDaemonClient::readSocket);
        connect(m_socket.get(), &QLocalSocket::bytesWritten, this,
                &SocketDaemonClient::writeSocket);
        connect(m_socket.get(), &QLocalSocket::disconnected, this,
                &SocketDaemonClient::socketDisconnected);
        connect(m_socket.get(), &QLocalSocket::errorOccurred, this,
                &SocketDaemonClient::socketError);
        m_socket->connectToServer(m_options.socketPath);
        if (!m_socket->waitForConnected(m_options.responseTimeoutMilliseconds))
        {
            const auto error =
                SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                     .detail = m_socket->errorString()};
            clearSocket(error.reason, error.detail);
            return error;
        }
        if (const auto error = validatePeerCredentials(*m_socket, m_options.enforcePeerCredentials))
        {
            clearSocket(error->reason, error->detail);
            return error;
        }
        m_lastError.reset();
        return std::nullopt;
    }

    void SocketDaemonClient::disconnectFromDaemon()
    {
        if (m_socket != nullptr)
            clearSocket(SocketDisconnectReason::PeerClosed, QStringLiteral("client disconnected"));
    }

    bool SocketDaemonClient::isConnected() const
    {
        return m_socket != nullptr && m_socket->state() == QLocalSocket::ConnectedState;
    }

    std::optional<SocketTransportError> SocketDaemonClient::lastError() const
    {
        return m_lastError;
    }

    std::optional<BoundaryError> SocketDaemonClient::attachEventSink(BoundaryEventSink& sink)
    {
        if (m_eventSink != nullptr && m_eventSink != &sink)
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("eventSink"),
                                 .detail = QStringLiteral("an event sink is already attached")};
        m_eventSink = &sink;
        return std::nullopt;
    }

    void SocketDaemonClient::detachEventSink(BoundaryEventSink& sink)
    {
        if (m_eventSink == &sink)
            m_eventSink = nullptr;
    }

    std::optional<SocketTransportError> SocketDaemonClient::ensureConnected()
    {
        if (isConnected())
            return std::nullopt;
        if (m_lastError.has_value())
            return m_lastError;
        return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                    .detail = QStringLiteral("socket client is not connected")};
    }

    std::optional<SocketTransportError> SocketDaemonClient::enqueue(PendingFrame frame)
    {
        const auto bytes = static_cast<std::size_t>(frame.data.size());
        if (m_pendingWrites.size() + (m_currentWrite != nullptr ? 1U : 0U) >=
                m_options.maximumQueuedFrames ||
            m_queuedBytes + bytes > m_options.maximumQueuedBytes)
        {
            return SocketTransportError{.reason = SocketDisconnectReason::QueueOverflow,
                                        .detail = QStringLiteral("socket output queue is full")};
        }
        m_queuedBytes += bytes;
        m_pendingWrites.push_back(std::move(frame));
        pumpWrites();
        return std::nullopt;
    }

    void SocketDaemonClient::readSocket()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket == nullptr)
            return;
        if (const auto error = m_decoder.append(m_socket->readAll()))
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation, error->detail);
            return;
        }
        while (m_socket != nullptr && !m_processingFrame)
        {
            auto decoded = m_decoder.takeFrame();
            if (auto* error = std::get_if<SocketFrameError>(&decoded))
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation, error->detail);
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

    void SocketDaemonClient::writeSocket(const qint64)
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        pumpWrites();
    }

    void SocketDaemonClient::socketDisconnected()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket != nullptr)
            clearSocket(SocketDisconnectReason::PeerClosed, QStringLiteral("daemon disconnected"));
    }

    void SocketDaemonClient::socketError()
    {
        const QScopedValueRollback callbackGuard{m_inSocketCallback, true};
        if (m_socket == nullptr)
            return;

        // QLocalSocket reports a waitForReadyRead() deadline as SocketTimeoutError even though the
        // local connection remains established and usable. The request-level deadline handles that
        // case; it is not a daemon-disconnect signal.
        if (m_socket->error() == QLocalSocket::SocketTimeoutError && isConnected())
            return;

        clearSocket(SocketDisconnectReason::TransportFailure, m_socket->errorString());
    }

    void SocketDaemonClient::handleFrame(const SocketFrame& frame)
    {
        if (frame.kind == SocketFrameKind::BoundaryEventFrame)
        {
            if (frame.correlation != 0)
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("boundary event correlation is non-zero"));
                return;
            }
            const auto event = decodeBoundaryEvent(frame.payload, m_options.limits);
            if (auto* error = std::get_if<SocketFrameError>(&event))
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation, error->detail);
                return;
            }
            // Boundary handlers may issue synchronous follow-up requests. Deliver outside the
            // socket read callback so their replies can be processed normally.
            auto boundaryEvent = std::get<BoundaryEvent>(std::move(event));
            QMetaObject::invokeMethod(
                this,
                [this, boundaryEvent = std::move(boundaryEvent)]
                {
                    if (m_eventSink != nullptr)
                        m_eventSink->onBoundaryEvent(boundaryEvent);
                },
                Qt::QueuedConnection);
            return;
        }
        if (frame.kind == SocketFrameKind::ProtocolError)
        {
            const auto error = decodeProtocolError(frame.payload, m_options.limits);
            if (auto* frameError = std::get_if<SocketFrameError>(&error))
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation, frameError->detail);
                return;
            }
            const auto detail =
                std::get<std::optional<BoundaryError>>(error)
                    .value_or(
                        BoundaryError{.code = BoundaryErrorCode::ProtocolViolation,
                                      .field = QStringLiteral("socket"),
                                      .detail = QStringLiteral("daemon rejected the socket frame")})
                    .detail;
            clearSocket(SocketDisconnectReason::ProtocolViolation, detail);
            return;
        }
        if (frame.correlation == 0)
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation,
                        QStringLiteral("reply correlation is zero"));
            return;
        }
        if (const auto pending = m_asyncReplies.find(frame.correlation);
            pending != m_asyncReplies.end())
        {
            if (frame.kind != pending->second->expectedKind)
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("socket reply kind is unexpected"));
                return;
            }
            auto reply = std::move(pending->second);
            m_asyncReplies.erase(pending);
            reply->promise.addResult(
                AsyncFrameResult{ReceivedFrame{.kind = frame.kind, .payload = frame.payload}});
            reply->promise.finish();
            return;
        }
        if (const auto deferred = m_abandonedReplies.find(frame.correlation);
            deferred != m_abandonedReplies.end())
        {
            if (frame.kind != deferred->second.expectedKind)
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("late socket reply kind is unexpected"));
                return;
            }
            if (!isResumableBootstrapRequest(deferred->second.requestKind))
            {
                m_abandonedReplies.erase(deferred);
                return;
            }
            if (deferred->second.received.has_value())
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("duplicate deferred socket reply"));
                return;
            }
            if (m_receivedBytes + static_cast<std::size_t>(frame.payload.size()) >
                m_options.maximumQueuedBytes)
            {
                clearSocket(SocketDisconnectReason::ProtocolViolation,
                            QStringLiteral("socket reply queue is full"));
                return;
            }
            m_receivedBytes += static_cast<std::size_t>(frame.payload.size());
            deferred->second.received = ReceivedFrame{.kind = frame.kind, .payload = frame.payload};
            Q_EMIT deferredReplyAvailable(deferred->second.requestKind);
            return;
        }
        if (m_receivedReplies.contains(frame.correlation))
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation,
                        QStringLiteral("duplicate socket reply correlation"));
            return;
        }
        if (m_receivedReplies.size() + m_asyncReplies.size() + m_abandonedReplies.size() >=
                m_options.maximumQueuedFrames ||
            m_receivedBytes + static_cast<std::size_t>(frame.payload.size()) >
                m_options.maximumQueuedBytes)
        {
            clearSocket(SocketDisconnectReason::ProtocolViolation,
                        QStringLiteral("socket reply queue is full"));
            return;
        }
        m_receivedBytes += static_cast<std::size_t>(frame.payload.size());
        m_receivedReplies.emplace(frame.correlation,
                                  ReceivedFrame{.kind = frame.kind, .payload = frame.payload});
    }

    void SocketDaemonClient::pumpWrites()
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
            return;
        m_currentWrite = std::make_unique<PendingFrame>(std::move(m_pendingWrites.front()));
        m_pendingWrites.pop_front();
        auto& frame = *m_currentWrite;
        while (frame.offset < static_cast<std::size_t>(frame.data.size()))
        {
            const auto remaining = frame.data.size() - static_cast<qsizetype>(frame.offset);
            const auto written = m_socket->write(frame.data.constData() + frame.offset, remaining);
            if (written < 0)
            {
                clearSocket(SocketDisconnectReason::TransportFailure, m_socket->errorString());
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

    void SocketDaemonClient::clearSocket(const SocketDisconnectReason reason, QString detail)
    {
        if (m_inSocketCallback)
        {
            if (m_clearScheduled)
                return;
            m_clearScheduled = true;
            m_lastError = SocketTransportError{.reason = reason, .detail = detail};
            QMetaObject::invokeMethod(
                this,
                [this, reason, detail = std::move(detail)]() mutable
                {
                    m_clearScheduled = false;
                    clearSocket(reason, std::move(detail));
                },
                Qt::QueuedConnection);
            return;
        }
        const bool wasConnected = m_socket != nullptr;
        if (m_socket != nullptr)
        {
            QSignalBlocker blocker{m_socket.get()};
            m_socket->abort();
        }
        m_socket.reset();
        m_decoder.clear();
        m_pendingWrites.clear();
        m_currentWrite.reset();
        m_receivedReplies.clear();
        auto asyncReplies = std::move(m_asyncReplies);
        m_asyncReplies.clear();
        m_abandonedReplies.clear();
        m_queuedBytes = 0;
        m_receivedBytes = 0;
        m_clearScheduled = false;
        m_lastError = SocketTransportError{.reason = reason, .detail = std::move(detail)};
        for (auto& [correlation, reply] : asyncReplies)
        {
            Q_UNUSED(correlation)
            reply->promise.addResult(AsyncFrameResult{*m_lastError});
            reply->promise.finish();
        }
        if (wasConnected)
            Q_EMIT connectionClosed(reason, m_lastError->detail);
    }

    std::variant<SocketDaemonClient::ReceivedFrame, SocketTransportError>
    SocketDaemonClient::request(const SocketFrameKind requestKind, const QByteArray& payload,
                                const SocketFrameKind replyKind)
    {
        if (const auto error = ensureConnected())
            return *error;

        std::optional<std::uint64_t> deferredCorrelation;
        if (isResumableBootstrapRequest(requestKind))
        {
            for (const auto& [correlation, deferred] : m_abandonedReplies)
            {
                if (deferred.requestKind == requestKind && deferred.expectedKind == replyKind &&
                    deferred.resumableRequestPayload.has_value() &&
                    *deferred.resumableRequestPayload == payload)
                {
                    deferredCorrelation = correlation;
                    break;
                }
            }
        }

        std::uint64_t correlation = 0;
        if (deferredCorrelation.has_value())
        {
            correlation = *deferredCorrelation;
        }
        else
        {
            if (m_receivedReplies.size() + m_asyncReplies.size() + m_abandonedReplies.size() >=
                m_options.maximumQueuedFrames)
            {
                return SocketTransportError{
                    .reason = SocketDisconnectReason::QueueOverflow,
                    .detail = QStringLiteral("too many socket replies are pending")};
            }
            correlation = m_nextCorrelation;
            ++m_nextCorrelation;
            if (m_nextCorrelation == 0)
                ++m_nextCorrelation;
            const auto frameData = encodeSocketFrame(requestKind, correlation, payload,
                                                     m_options.limits.maximumFrameBytes);
            if (auto* error = std::get_if<SocketFrameError>(&frameData))
                return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                            .detail = error->detail};
            if (const auto error = enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                                        .kind = requestKind,
                                                        .correlation = correlation}))
                return *error;
        }

        if (const auto error = waitForReply(correlation, requestKind, payload, replyKind))
            return *error;

        if (const auto iterator = m_receivedReplies.find(correlation);
            iterator != m_receivedReplies.end())
        {
            auto reply = std::move(iterator->second);
            m_receivedBytes -= static_cast<std::size_t>(reply.payload.size());
            m_receivedReplies.erase(iterator);
            if (reply.kind != replyKind)
                return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                            .detail =
                                                QStringLiteral("socket reply kind is unexpected")};
            return reply;
        }

        if (const auto deferred = m_abandonedReplies.find(correlation);
            deferred != m_abandonedReplies.end() && deferred->second.received.has_value())
        {
            auto reply = std::move(*deferred->second.received);
            m_receivedBytes -= static_cast<std::size_t>(reply.payload.size());
            m_abandonedReplies.erase(deferred);
            if (reply.kind != replyKind)
                return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                            .detail =
                                                QStringLiteral("socket reply kind is unexpected")};
            return reply;
        }

        return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                    .detail = QStringLiteral("socket reply was lost")};
    }

    QFuture<SocketDaemonClient::AsyncFrameResult>
    SocketDaemonClient::requestAsync(const SocketFrameKind requestKind, const QByteArray& payload,
                                     const SocketFrameKind replyKind)
    {
        auto pending = std::make_unique<PendingAsyncReply>();
        pending->expectedKind = replyKind;
        pending->promise.start();
        auto future = pending->promise.future();
        const auto failImmediately = [&pending](SocketTransportError error)
        {
            pending->promise.addResult(AsyncFrameResult{std::move(error)});
            pending->promise.finish();
        };

        if (const auto error = ensureConnected())
        {
            failImmediately(*error);
            return future;
        }
        if (m_asyncReplies.size() + m_receivedReplies.size() + m_abandonedReplies.size() >=
            m_options.maximumQueuedFrames)
        {
            failImmediately({.reason = SocketDisconnectReason::QueueOverflow,
                             .detail = QStringLiteral("too many socket replies are pending")});
            return future;
        }
        const auto correlation = m_nextCorrelation;
        ++m_nextCorrelation;
        if (m_nextCorrelation == 0)
            ++m_nextCorrelation;
        const auto frameData = encodeSocketFrame(requestKind, correlation, payload,
                                                 m_options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&frameData))
        {
            failImmediately(
                {.reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
            return future;
        }
        m_asyncReplies.emplace(correlation, std::move(pending));
        if (const auto error = enqueue(PendingFrame{.data = std::get<QByteArray>(frameData),
                                                    .kind = requestKind,
                                                    .correlation = correlation}))
        {
            if (const auto found = m_asyncReplies.find(correlation); found != m_asyncReplies.end())
            {
                auto failed = std::move(found->second);
                m_asyncReplies.erase(found);
                failed->promise.addResult(AsyncFrameResult{*error});
                failed->promise.finish();
            }
            return future;
        }
        return future;
    }

    std::optional<SocketTransportError> SocketDaemonClient::waitForReply(
        const std::uint64_t correlation, const SocketFrameKind requestKind,
        const QByteArray& requestPayload, const SocketFrameKind replyKind)
    {
        QElapsedTimer timer;
        timer.start();
        while (true)
        {
            if (m_lastError.has_value())
                return m_lastError;
            if (const auto iterator = m_receivedReplies.find(correlation);
                iterator != m_receivedReplies.end())
            {
                if (iterator->second.kind != replyKind)
                    return SocketTransportError{
                        .reason = SocketDisconnectReason::ProtocolViolation,
                        .detail = QStringLiteral("socket reply kind is unexpected")};
                return std::nullopt;
            }
            if (const auto deferred = m_abandonedReplies.find(correlation);
                deferred != m_abandonedReplies.end() && deferred->second.received.has_value())
            {
                if (deferred->second.received->kind != replyKind)
                    return SocketTransportError{
                        .reason = SocketDisconnectReason::ProtocolViolation,
                        .detail = QStringLiteral("socket reply kind is unexpected")};
                return std::nullopt;
            }
            if (!isConnected())
                return m_lastError.value_or(SocketTransportError{
                    .reason = SocketDisconnectReason::PeerClosed,
                    .detail = QStringLiteral("daemon disconnected before the reply")});
            const auto elapsed = static_cast<int>(timer.elapsed());
            const auto remaining = m_options.responseTimeoutMilliseconds - elapsed;
            if (remaining <= 0 || !m_socket->waitForReadyRead(remaining))
            {
                if (!isConnected())
                {
                    return m_lastError.value_or(SocketTransportError{
                        .reason = SocketDisconnectReason::PeerClosed,
                        .detail = QStringLiteral("daemon disconnected before the reply")});
                }
                m_abandonedReplies.try_emplace(
                    correlation, DeferredReply{.requestKind = requestKind,
                                               .expectedKind = replyKind,
                                               .resumableRequestPayload =
                                                   isResumableBootstrapRequest(requestKind)
                                                       ? std::optional<QByteArray>{requestPayload}
                                                       : std::nullopt,
                                               .received = std::nullopt});
                return SocketTransportError{
                    .reason = SocketDisconnectReason::TransportFailure,
                    .detail = QStringLiteral(
                        "timed out waiting for a daemon reply while the socket remained connected"),
                };
            }
            readSocket();
        }
    }

    BoundaryError SocketDaemonClient::boundaryError(const SocketTransportError& error) const
    {
        return makeBoundaryError(error);
    }

    CommandReply SocketDaemonClient::submitCommand(CommandRequest request)
    {
        const auto id = request.id;
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return CommandRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = this->request(std::get<EncodedPayload>(encoded).kind,
                                          std::get<EncodedPayload>(encoded).payload,
                                          SocketFrameKind::CommandReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return CommandRejected{.id = id, .error = boundaryError(*error)};
        const auto reply =
            decodeCommandReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return CommandRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<CommandReply>(reply);
    }

    QFuture<CommandReply> SocketDaemonClient::submitCommandAsync(CommandRequest request)
    {
        const auto id = request.id;
        auto promise = std::make_shared<QPromise<CommandReply>>();
        promise->start();
        auto future = promise->future();
        const auto complete = [promise](CommandReply reply)
        {
            promise->addResult(std::move(reply));
            promise->finish();
        };

        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
        {
            complete(CommandRejected{.id = id,
                                     .error = boundaryError(SocketTransportError{
                                         .reason = SocketDisconnectReason::ProtocolViolation,
                                         .detail = error->detail})});
            return future;
        }

        auto* watcher = new QFutureWatcher<AsyncFrameResult>(this);
        connect(watcher, &QFutureWatcherBase::finished, this,
                [this, watcher, id, complete]
                {
                    const auto result = watcher->result();
                    watcher->deleteLater();
                    if (const auto* error = std::get_if<SocketTransportError>(&result))
                    {
                        complete(CommandRejected{.id = id, .error = boundaryError(*error)});
                        return;
                    }
                    const auto reply = decodeCommandReply(std::get<ReceivedFrame>(result).payload,
                                                          m_options.limits);
                    if (const auto* error = std::get_if<SocketFrameError>(&reply))
                    {
                        complete(
                            CommandRejected{.id = id,
                                            .error = boundaryError(SocketTransportError{
                                                .reason = SocketDisconnectReason::ProtocolViolation,
                                                .detail = error->detail})});
                        return;
                    }
                    complete(std::get<CommandReply>(reply));
                });
        watcher->setFuture(requestAsync(std::get<EncodedPayload>(encoded).kind,
                                        std::get<EncodedPayload>(encoded).payload,
                                        SocketFrameKind::CommandReplyFrame));
        return future;
    }

    MaterializationReply SocketDaemonClient::requestMaterialization(MaterializationRequest request)
    {
        const auto id = request.id;
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return MaterializationRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = this->request(std::get<EncodedPayload>(encoded).kind,
                                          std::get<EncodedPayload>(encoded).payload,
                                          SocketFrameKind::MaterializationReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return MaterializationRejected{.id = id, .error = boundaryError(*error)};
        const auto reply =
            decodeMaterializationReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return MaterializationRejected{
                .id = id,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<MaterializationReply>(reply);
    }

    void SocketDaemonClient::cancelMaterializationScope(const ScopeId scope)
    {
        const auto encoded = encodeClientRequest(
            ClientRequest{CancelMaterializationScopeRequest{.scope = scope}}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            return;
        }
        const auto result = request(std::get<EncodedPayload>(encoded).kind,
                                    std::get<EncodedPayload>(encoded).payload,
                                    SocketFrameKind::CancelMaterializationScopeReply);
        if (std::holds_alternative<SocketTransportError>(result))
            return;
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
    }

    SettingsReadReply SocketDaemonClient::getSettings()
    {
        const auto encoded =
            encodeClientRequest(ClientRequest{GetSettingsRequest{}}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return SettingsReadRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = request(std::get<EncodedPayload>(encoded).kind,
                                    std::get<EncodedPayload>(encoded).payload,
                                    SocketFrameKind::SettingsReadReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return SettingsReadRejected{.error = boundaryError(*error)};
        const auto reply =
            decodeSettingsReadReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return SettingsReadRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<SettingsReadReply>(reply);
    }

    SettingsUpdateReply SocketDaemonClient::updateSettings(UpdateSettingsRequest request)
    {
        const auto baseRevision = request.baseRevision;
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return SettingsUpdateRejected{
                .currentRevision = baseRevision,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result = this->request(std::get<EncodedPayload>(encoded).kind,
                                          std::get<EncodedPayload>(encoded).payload,
                                          SocketFrameKind::SettingsUpdateReplyFrame);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return SettingsUpdateRejected{.currentRevision = baseRevision,
                                          .error = boundaryError(*error)};
        const auto reply =
            decodeSettingsUpdateReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return SettingsUpdateRejected{
                .currentRevision = baseRevision,
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        return std::get<SettingsUpdateReply>(reply);
    }

    HandshakeReply SocketDaemonClient::hello(HelloRequest request)
    {
        const auto encoded = encodeClientRequest(ClientRequest{request}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return HandshakeRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto result =
            this->request(std::get<EncodedPayload>(encoded).kind,
                          std::get<EncodedPayload>(encoded).payload, SocketFrameKind::HelloReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return HandshakeRejected{.error = boundaryError(*error)};
        const auto reply =
            decodeHandshakeReply(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return HandshakeRejected{
                .error = boundaryError(SocketTransportError{
                    .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail})};
        const auto& handshake = std::get<HandshakeReply>(reply);
        if (const auto* ready = std::get_if<ReadyReply>(&handshake);
            ready != nullptr && ready->protocol.major != request.protocol.major)
            return HandshakeRejected{
                .error = {.code = BoundaryErrorCode::IncompatibleBuild,
                          .field = QStringLiteral("hello.protocol"),
                          .detail = QStringLiteral("daemon returned an incompatible protocol")}};
        return handshake;
    }

    std::optional<BoundaryError> SocketDaemonClient::ping()
    {
        const auto encoded = encodeClientRequest(ClientRequest{PingRequest{}}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        const auto result =
            request(std::get<EncodedPayload>(encoded).kind,
                    std::get<EncodedPayload>(encoded).payload, SocketFrameKind::PingReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return boundaryError(*error);
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        return std::get<std::optional<BoundaryError>>(reply);
    }

    std::optional<BoundaryError> SocketDaemonClient::readyForActivation()
    {
        const auto result = request(SocketFrameKind::ReadyForActivationRequest, {},
                                    SocketFrameKind::ReadyForActivationReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return boundaryError(*error);
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        return std::get<std::optional<BoundaryError>>(reply);
    }

    std::optional<BoundaryError> SocketDaemonClient::acknowledgeCacheAccessSuspended(
        CacheAccessSuspendedAcknowledgement acknowledgement)
    {
        const auto encoded = encodeClientRequest(ClientRequest{acknowledgement}, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        const auto result = request(std::get<EncodedPayload>(encoded).kind,
                                    std::get<EncodedPayload>(encoded).payload,
                                    SocketFrameKind::CacheAccessSuspendedReply);
        if (auto* error = std::get_if<SocketTransportError>(&result))
            return boundaryError(*error);
        const auto reply =
            decodeOptionalError(std::get<ReceivedFrame>(result).payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return boundaryError(SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation, .detail = error->detail});
        return std::get<std::optional<BoundaryError>>(reply);
    }

} // namespace javelin::protocol
