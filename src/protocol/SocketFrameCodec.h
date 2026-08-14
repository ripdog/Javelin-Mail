#pragma once

#include "protocol/ActivationContract.h"
#include "protocol/ProtocolTypes.h"

#include <QByteArray>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace javelin::protocol
{
    enum class SocketFrameKind : std::uint16_t
    {
        HelloRequest = 1,
        CommandRequest = 2,
        MaterializationRequest = 3,
        CancelMaterializationScopeRequest = 4,
        GetSettingsRequest = 5,
        UpdateSettingsRequest = 6,
        CacheAccessSuspendedAcknowledgement = 7,
        PingRequest = 8,
        ReadyForActivationRequest = 9,
        ActivationRequest = 10,
        HelloReply = 32,
        CommandReplyFrame = 33,
        MaterializationReplyFrame = 34,
        CancelMaterializationScopeReply = 35,
        SettingsReadReplyFrame = 36,
        SettingsUpdateReplyFrame = 37,
        CacheAccessSuspendedReply = 38,
        PingReply = 39,
        ReadyForActivationReply = 40,
        ActivationReply = 41,
        BoundaryEventFrame = 64,
        ProtocolError = 65,
    };

    enum class SocketFrameErrorCode : std::uint8_t
    {
        InvalidHeader,
        UnsupportedVersion,
        UnknownMessageKind,
        FrameTooLarge,
        MalformedPayload,
    };

    struct SocketFrameError
    {
        SocketFrameErrorCode code = SocketFrameErrorCode::InvalidHeader;
        QString detail;
    };

    struct SocketFrame
    {
        SocketFrameKind kind = SocketFrameKind::ProtocolError;
        std::uint64_t correlation = 0;
        QByteArray payload;
    };

    class SocketFrameDecoder final
    {
      public:
        explicit SocketFrameDecoder(BoundaryLimits limits = {});

        [[nodiscard]] std::optional<SocketFrameError> append(const QByteArray& bytes);
        [[nodiscard]] std::variant<std::optional<SocketFrame>, SocketFrameError> takeFrame();
        void clear();

      private:
        QByteArray m_buffer;
        BoundaryLimits m_limits;
    };

    [[nodiscard]] bool isKnownSocketFrameKind(std::uint16_t kind);
    [[nodiscard]] std::variant<QByteArray, SocketFrameError>
    encodeSocketFrame(SocketFrameKind kind, std::uint64_t correlation, const QByteArray& payload,
                      std::size_t maximumFrameBytes = BoundaryLimits{}.maximumFrameBytes);

    [[nodiscard]] std::variant<QByteArray, SocketFrameError>
    encodeActivationRoute(const ActivationRoute& route, const BoundaryLimits& limits = {});
    [[nodiscard]] std::variant<ActivationRoute, SocketFrameError>
    decodeActivationRoute(const QByteArray& payload, const BoundaryLimits& limits = {});
    [[nodiscard]] std::variant<QByteArray, SocketFrameError>
    encodeActivationReply(const std::optional<BoundaryError>& error,
                          const BoundaryLimits& limits = {});
    [[nodiscard]] std::variant<std::optional<BoundaryError>, SocketFrameError>
    decodeActivationReply(const QByteArray& payload, const BoundaryLimits& limits = {});
} // namespace javelin::protocol
