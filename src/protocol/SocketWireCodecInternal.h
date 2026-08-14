#pragma once

#include "protocol/BoundaryEventContract.h"
#include "protocol/DaemonRequestHandler.h"
#include "protocol/ProtocolValidation.h"
#include "protocol/SocketFrameCodec.h"
#include "protocol/SocketSecurity.h"

#include <QByteArray>

#include <optional>
#include <variant>

class QLocalServer;
class QLocalSocket;

namespace javelin::protocol::detail
{
    struct EncodedPayload
    {
        SocketFrameKind kind = SocketFrameKind::ProtocolError;
        QByteArray payload;
    };

    using EncodedPayloadResult = std::variant<EncodedPayload, SocketFrameError>;

    struct DecodedActivationRequest
    {
        ProtocolVersion protocol;
        BuildIdentity build;
        ActivationRoute route;
    };

    [[nodiscard]] EncodedPayloadResult encodeClientRequest(const ClientRequest& request,
                                                           const BoundaryLimits& limits);
    [[nodiscard]] std::variant<ClientRequest, SocketFrameError>
    decodeClientRequest(SocketFrameKind kind, const QByteArray& payload,
                        const BoundaryLimits& limits);

    [[nodiscard]] EncodedPayloadResult encodeHandshakeReply(const HandshakeReply& reply,
                                                            const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult encodeCommandReply(const CommandReply& reply,
                                                          const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult encodeMaterializationReply(const MaterializationReply& reply,
                                                                  const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult encodeSettingsReadReply(const SettingsReadReply& reply,
                                                               const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult encodeSettingsUpdateReply(const SettingsUpdateReply& reply,
                                                                 const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult
    encodeOptionalError(SocketFrameKind kind, const std::optional<BoundaryError>& error,
                        const BoundaryLimits& limits);

    [[nodiscard]] std::variant<HandshakeReply, SocketFrameError>
    decodeHandshakeReply(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] std::variant<CommandReply, SocketFrameError>
    decodeCommandReply(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] std::variant<MaterializationReply, SocketFrameError>
    decodeMaterializationReply(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] std::variant<SettingsReadReply, SocketFrameError>
    decodeSettingsReadReply(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] std::variant<SettingsUpdateReply, SocketFrameError>
    decodeSettingsUpdateReply(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] std::variant<std::optional<BoundaryError>, SocketFrameError>
    decodeOptionalError(const QByteArray& payload, const BoundaryLimits& limits);

    [[nodiscard]] EncodedPayloadResult encodeActivationRequest(const SocketEndpointOptions& options,
                                                               const ActivationRoute& route);
    [[nodiscard]] std::variant<DecodedActivationRequest, SocketFrameError>
    decodeActivationRequest(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult encodeBoundaryEvent(const BoundaryEvent& event,
                                                           const BoundaryLimits& limits);
    [[nodiscard]] std::variant<BoundaryEvent, SocketFrameError>
    decodeBoundaryEvent(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] std::variant<SocketFrameError, std::optional<BoundaryError>>
    decodeProtocolError(const QByteArray& payload, const BoundaryLimits& limits);
    [[nodiscard]] EncodedPayloadResult encodeProtocolError(const SocketFrameError& error,
                                                           const BoundaryLimits& limits);

    [[nodiscard]] std::optional<SocketTransportError>
    validateRuntimeDirectory(const SocketEndpointOptions& options);
    [[nodiscard]] std::optional<SocketTransportError>
    listenOnLocalServer(QLocalServer& server, const SocketEndpointOptions& options,
                        QString addressInUseDetail);
    void closeLocalServer(QLocalServer* server, const QString& socketPath);
    [[nodiscard]] std::optional<SocketTransportError> validatePeerCredentials(QLocalSocket& socket,
                                                                              bool enforce);
    [[nodiscard]] BoundaryError makeBoundaryError(const SocketTransportError& error);
    [[nodiscard]] bool isReadyReply(const HandshakeReply& reply);
    [[nodiscard]] bool sameEventKind(const std::optional<BoundaryEvent>& left,
                                     const std::optional<BoundaryEvent>& right);
    void mergeInvalidation(CacheInvalidation& target, const CacheInvalidation& source,
                           const BoundaryLimits& limits);
} // namespace javelin::protocol::detail
