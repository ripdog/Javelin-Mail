#include "protocol/SocketFrameCodec.h"

#include <cstring>
#include <limits>

namespace javelin::protocol
{
    namespace
    {
        constexpr std::size_t frameHeaderBytes = 24;
        constexpr quint16 wireVersion = 2;
        constexpr char frameMagic[] = {'J', 'V', 'I', 'P'};

        [[nodiscard]] quint16 readU16(const QByteArray& bytes, const int offset)
        {
            const auto high =
                static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(offset)));
            const auto low =
                static_cast<unsigned int>(static_cast<unsigned char>(bytes.at(offset + 1)));
            return static_cast<quint16>((high << 8U) | low);
        }

        [[nodiscard]] quint32 readU32(const QByteArray& bytes, const int offset)
        {
            quint32 value = 0;
            for (int index = 0; index < 4; ++index)
                value = (value << 8) |
                        static_cast<quint32>(static_cast<unsigned char>(bytes.at(offset + index)));
            return value;
        }

        [[nodiscard]] quint64 readU64(const QByteArray& bytes, const int offset)
        {
            quint64 value = 0;
            for (int index = 0; index < 8; ++index)
                value = (value << 8) |
                        static_cast<quint64>(static_cast<unsigned char>(bytes.at(offset + index)));
            return value;
        }

        void writeU16(char* target, const quint16 value)
        {
            target[0] = static_cast<char>((value >> 8) & 0xff);
            target[1] = static_cast<char>(value & 0xff);
        }

        void writeU32(char* target, const quint32 value)
        {
            for (int index = 0; index < 4; ++index)
                target[index] = static_cast<char>((value >> (24 - index * 8)) & 0xff);
        }

        void writeU64(char* target, const quint64 value)
        {
            for (int index = 0; index < 8; ++index)
                target[index] = static_cast<char>((value >> (56 - index * 8)) & 0xff);
        }
    } // namespace

    SocketFrameDecoder::SocketFrameDecoder(BoundaryLimits limits) : m_limits(limits)
    {
    }

    std::optional<SocketFrameError> SocketFrameDecoder::append(const QByteArray& bytes)
    {
        const auto maximumBuffer =
            m_limits.maximumFrameBytes > std::numeric_limits<std::size_t>::max() / 2
                ? std::numeric_limits<std::size_t>::max()
                : m_limits.maximumFrameBytes * 2;
        if (static_cast<std::size_t>(m_buffer.size()) + static_cast<std::size_t>(bytes.size()) >
            maximumBuffer)
        {
            return SocketFrameError{.code = SocketFrameErrorCode::FrameTooLarge,
                                    .detail = QStringLiteral("socket input buffer is too large")};
        }
        m_buffer.append(bytes);
        return std::nullopt;
    }

    std::variant<std::optional<SocketFrame>, SocketFrameError> SocketFrameDecoder::takeFrame()
    {
        if (m_buffer.size() < static_cast<qsizetype>(frameHeaderBytes))
            return std::optional<SocketFrame>{};
        if (std::memcmp(m_buffer.constData(), frameMagic, sizeof(frameMagic)) != 0)
            return SocketFrameError{.code = SocketFrameErrorCode::InvalidHeader,
                                    .detail = QStringLiteral("socket frame magic is invalid")};
        if (readU16(m_buffer, 4) != wireVersion)
            return SocketFrameError{.code = SocketFrameErrorCode::UnsupportedVersion,
                                    .detail = QStringLiteral("socket wire version is unsupported")};
        if (readU16(m_buffer, 8) != 0 || readU16(m_buffer, 10) != 0)
            return SocketFrameError{.code = SocketFrameErrorCode::InvalidHeader,
                                    .detail = QStringLiteral("socket frame flags are invalid")};
        const auto rawKind = readU16(m_buffer, 6);
        if (!isKnownSocketFrameKind(rawKind))
            return SocketFrameError{.code = SocketFrameErrorCode::UnknownMessageKind,
                                    .detail = QStringLiteral("socket message kind is unknown")};
        const auto payloadLength = static_cast<std::size_t>(readU32(m_buffer, 12));
        if (m_limits.maximumFrameBytes < frameHeaderBytes ||
            payloadLength > m_limits.maximumFrameBytes - frameHeaderBytes)
            return SocketFrameError{.code = SocketFrameErrorCode::FrameTooLarge,
                                    .detail =
                                        QStringLiteral("socket frame exceeds the size limit")};
        const auto frameLength = frameHeaderBytes + payloadLength;
        if (static_cast<std::size_t>(m_buffer.size()) < frameLength)
            return std::optional<SocketFrame>{};
        SocketFrame frame{.kind = static_cast<SocketFrameKind>(rawKind),
                          .correlation = readU64(m_buffer, 16),
                          .payload = m_buffer.mid(static_cast<qsizetype>(frameHeaderBytes),
                                                  static_cast<qsizetype>(payloadLength))};
        m_buffer.remove(0, static_cast<qsizetype>(frameLength));
        return frame;
    }

    void SocketFrameDecoder::clear()
    {
        m_buffer.clear();
    }

    bool isKnownSocketFrameKind(const std::uint16_t kind)
    {
        switch (static_cast<SocketFrameKind>(kind))
        {
        case SocketFrameKind::HelloRequest:
        case SocketFrameKind::CommandRequest:
        case SocketFrameKind::MaterializationRequest:
        case SocketFrameKind::CancelMaterializationScopeRequest:
        case SocketFrameKind::GetSettingsRequest:
        case SocketFrameKind::UpdateSettingsRequest:
        case SocketFrameKind::CacheAccessSuspendedAcknowledgement:
        case SocketFrameKind::PingRequest:
        case SocketFrameKind::ReadyForActivationRequest:
        case SocketFrameKind::ActivationRequest:
        case SocketFrameKind::HelloReply:
        case SocketFrameKind::CommandReplyFrame:
        case SocketFrameKind::MaterializationReplyFrame:
        case SocketFrameKind::CancelMaterializationScopeReply:
        case SocketFrameKind::SettingsReadReplyFrame:
        case SocketFrameKind::SettingsUpdateReplyFrame:
        case SocketFrameKind::CacheAccessSuspendedReply:
        case SocketFrameKind::PingReply:
        case SocketFrameKind::ReadyForActivationReply:
        case SocketFrameKind::ActivationReply:
        case SocketFrameKind::BoundaryEventFrame:
        case SocketFrameKind::ProtocolError:
            return true;
        }
        return false;
    }

    std::variant<QByteArray, SocketFrameError>
    encodeSocketFrame(const SocketFrameKind kind, const std::uint64_t correlation,
                      const QByteArray& payload, const std::size_t maximumFrameBytes)
    {
        if (!isKnownSocketFrameKind(static_cast<std::uint16_t>(kind)))
            return SocketFrameError{.code = SocketFrameErrorCode::UnknownMessageKind,
                                    .detail = QStringLiteral("socket message kind is unknown")};
        if (maximumFrameBytes < frameHeaderBytes ||
            static_cast<std::size_t>(payload.size()) > maximumFrameBytes - frameHeaderBytes)
            return SocketFrameError{.code = SocketFrameErrorCode::FrameTooLarge,
                                    .detail =
                                        QStringLiteral("socket frame exceeds the size limit")};
        QByteArray frame;
        frame.resize(static_cast<qsizetype>(frameHeaderBytes));
        std::memcpy(frame.data(), frameMagic, sizeof(frameMagic));
        writeU16(frame.data() + 4, wireVersion);
        writeU16(frame.data() + 6, static_cast<quint16>(kind));
        writeU16(frame.data() + 8, 0);
        writeU16(frame.data() + 10, 0);
        writeU32(frame.data() + 12, static_cast<quint32>(payload.size()));
        writeU64(frame.data() + 16, correlation);
        frame.append(payload);
        return frame;
    }
} // namespace javelin::protocol
