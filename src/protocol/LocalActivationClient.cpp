#include "protocol/LocalActivationClient.h"
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
    SocketActivationResult SocketActivationClient::request(const SocketClientOptions& options,
                                                           ActivationRoute route)
    {
        if (const auto error = validateRuntimeDirectory(options))
            return *error;
        QLocalSocket socket;
        socket.connectToServer(options.socketPath);
        if (!socket.waitForConnected(options.responseTimeoutMilliseconds))
            return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                        .detail = socket.errorString()};
        if (const auto error = validatePeerCredentials(socket, options.enforcePeerCredentials))
            return *error;
        const auto payload = encodeActivationRequest(options, route);
        if (auto* error = std::get_if<SocketFrameError>(&payload))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto frame = encodeSocketFrame(SocketFrameKind::ActivationRequest, 1,
                                             std::get<EncodedPayload>(payload).payload,
                                             options.limits.maximumFrameBytes);
        if (auto* error = std::get_if<SocketFrameError>(&frame))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto bytes = std::get<QByteArray>(frame);
        if (socket.write(bytes) != bytes.size() ||
            !socket.waitForBytesWritten(options.responseTimeoutMilliseconds) ||
            !socket.waitForReadyRead(options.responseTimeoutMilliseconds))
            return SocketTransportError{
                .reason = SocketDisconnectReason::TransportFailure,
                .detail = socket.errorString().isEmpty()
                              ? QStringLiteral("activation reply was not received")
                              : socket.errorString()};
        SocketFrameDecoder decoder{options.limits};
        if (const auto error = decoder.append(socket.readAll()))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto decoded = decoder.takeFrame();
        if (auto* error = std::get_if<SocketFrameError>(&decoded))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        const auto& response = std::get<std::optional<SocketFrame>>(decoded);
        if (!response.has_value() || response->kind != SocketFrameKind::ActivationReply ||
            response->correlation != 1)
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = QStringLiteral("invalid activation reply")};
        const auto reply = decodeActivationReply(response->payload, options.limits);
        if (auto* error = std::get_if<SocketFrameError>(&reply))
            return SocketTransportError{.reason = SocketDisconnectReason::ProtocolViolation,
                                        .detail = error->detail};
        return std::get<std::optional<BoundaryError>>(reply);
    }

} // namespace javelin::protocol
