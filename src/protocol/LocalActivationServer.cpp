#include "protocol/LocalActivationServer.h"
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
    SocketActivationEndpoint::SocketActivationEndpoint(ActivationRequestHandler& handler,
                                                       SocketEndpointOptions options,
                                                       QObject* parent)
        : QObject(parent), m_handler(handler), m_options(std::move(options)),
          m_decoder(m_options.limits)
    {
    }

    SocketActivationEndpoint::~SocketActivationEndpoint()
    {
        close();
    }

    std::optional<SocketTransportError> SocketActivationEndpoint::listen()
    {
        if (m_server == nullptr)
        {
            m_server = std::make_unique<QLocalServer>();
            m_server->setSocketOptions(QLocalServer::UserAccessOption);
            connect(m_server.get(), &QLocalServer::newConnection, this,
                    &SocketActivationEndpoint::acceptConnection);
        }
        m_lastError = listenOnLocalServer(
            *m_server, m_options, QStringLiteral("activation socket path is already in use"));
        return m_lastError;
    }

    void SocketActivationEndpoint::close()
    {
        clearSocket();
        closeLocalServer(m_server.get(), m_options.socketPath);
    }

    void SocketActivationEndpoint::acceptConnection()
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
                m_lastError = error;
                candidate->disconnectFromServer();
                candidate->deleteLater();
                continue;
            }
            candidate->setReadBufferSize(static_cast<qint64>(m_options.limits.maximumFrameBytes));
            m_socket.reset(candidate);
            connect(m_socket.get(), &QLocalSocket::readyRead, this,
                    &SocketActivationEndpoint::readSocket);
            connect(m_socket.get(), &QLocalSocket::disconnected, this,
                    &SocketActivationEndpoint::socketDisconnected);
            connect(m_socket.get(), &QLocalSocket::errorOccurred, this,
                    &SocketActivationEndpoint::socketError);
        }
    }

    void SocketActivationEndpoint::readSocket()
    {
        if (m_socket == nullptr)
            return;
        if (const auto error = m_decoder.append(m_socket->readAll()))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        auto decoded = m_decoder.takeFrame();
        if (auto* error = std::get_if<SocketFrameError>(&decoded))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        const auto& frame = std::get<std::optional<SocketFrame>>(decoded);
        if (!frame.has_value())
            return;
        if (frame->kind != SocketFrameKind::ActivationRequest || frame->correlation == 0)
        {
            m_lastError = SocketTransportError{
                .reason = SocketDisconnectReason::ProtocolViolation,
                .detail = QStringLiteral("invalid activation request frame"),
            };
            clearSocket();
            return;
        }
        const auto request = decodeActivationRequest(frame->payload, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&request))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        const auto& requestValue = std::get<DecodedActivationRequest>(request);
        std::optional<BoundaryError> activationError;
        if (requestValue.protocol.major != m_options.protocol.major ||
            requestValue.protocol.minor > m_options.protocol.minor)
        {
            activationError = BoundaryError{
                .code = BoundaryErrorCode::InvalidProtocol,
                .field = QStringLiteral("activation.protocol"),
                .detail = QStringLiteral("daemon protocol is incompatible"),
            };
        }
        else if (m_options.expectedBuild.has_value() &&
                 requestValue.build != *m_options.expectedBuild)
        {
            activationError = BoundaryError{
                .code = BoundaryErrorCode::IncompatibleBuild,
                .field = QStringLiteral("activation.build"),
                .detail = QStringLiteral("daemon build identity is incompatible"),
            };
        }
        else
        {
            activationError = m_handler.handleGuiActivation(requestValue.route);
        }
        const auto payload = encodeActivationReply(activationError, m_options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&payload))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        const auto encoded =
            encodeSocketFrame(SocketFrameKind::ActivationReply, frame->correlation,
                              std::get<QByteArray>(payload), m_options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&encoded))
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                               .detail = error->detail};
            clearSocket();
            return;
        }
        m_socket->write(std::get<QByteArray>(encoded));
        m_socket->flush();
        finishSocket();
    }

    void SocketActivationEndpoint::socketDisconnected()
    {
        clearSocket();
    }

    void SocketActivationEndpoint::socketError()
    {
        if (m_socket != nullptr)
        {
            m_lastError = SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                               .detail = m_socket->errorString()};
            clearSocket();
        }
    }

    void SocketActivationEndpoint::finishSocket()
    {
        auto* socket = m_socket.release();
        m_decoder.clear();
        if (socket == nullptr)
            return;

        QObject::disconnect(socket, nullptr, this, nullptr);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        socket->disconnectFromServer();
        if (socket->state() == QLocalSocket::UnconnectedState)
            socket->deleteLater();
    }

    void SocketActivationEndpoint::clearSocket()
    {
        auto* socket = m_socket.release();
        m_decoder.clear();
        if (socket == nullptr)
            return;
        QSignalBlocker blocker{socket};
        socket->abort();
        socket->deleteLater();
    }

} // namespace javelin::protocol
