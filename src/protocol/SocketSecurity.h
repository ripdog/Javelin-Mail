#pragma once

#include "protocol/ProtocolTypes.h"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace javelin::protocol
{
    enum class SocketDisconnectReason : std::uint8_t
    {
        None,
        PeerClosed,
        ProtocolViolation,
        IncompatiblePeer,
        QueueOverflow,
        TransportFailure,
        CredentialFailure,
        ReplacedPeer,
        ServerShutdown,
    };

    struct SocketTransportError
    {
        SocketDisconnectReason reason = SocketDisconnectReason::TransportFailure;
        QString detail;
    };

    struct SocketEndpointOptions
    {
        QString runtimeDirectory;
        QString socketPath;
        BoundaryLimits limits;
        ProtocolVersion protocol;
        std::optional<BuildIdentity> expectedBuild;
        std::size_t maximumQueuedFrames = 128;
        std::size_t maximumQueuedBytes = 4 * 1024 * 1024;
        int responseTimeoutMilliseconds = 5000;
        bool enforcePeerCredentials = true;
    };

    using SocketClientOptions = SocketEndpointOptions;
} // namespace javelin::protocol
