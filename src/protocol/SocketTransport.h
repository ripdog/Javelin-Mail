#pragma once

#include "protocol/ProcessBoundary.h"

#include <QByteArray>
#include <QFuture>
#include <QObject>
#include <QPromise>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

class QLocalServer;
class QLocalSocket;

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
        [[nodiscard]] std::size_t bufferedBytes() const;
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

    class SocketDaemonEndpoint final : public QObject
    {
        Q_OBJECT

      public:
        explicit SocketDaemonEndpoint(DaemonRequestHandler& handler,
                                      SocketEndpointOptions options = {},
                                      QObject* parent = nullptr);
        ~SocketDaemonEndpoint() override;

        SocketDaemonEndpoint(const SocketDaemonEndpoint&) = delete;
        SocketDaemonEndpoint& operator=(const SocketDaemonEndpoint&) = delete;
        SocketDaemonEndpoint(SocketDaemonEndpoint&&) = delete;
        SocketDaemonEndpoint& operator=(SocketDaemonEndpoint&&) = delete;

        [[nodiscard]] std::optional<SocketTransportError> listen();
        void close();
        [[nodiscard]] bool isListening() const;
        [[nodiscard]] const QString& socketPath() const;
        [[nodiscard]] std::optional<SocketTransportError> lastError() const;

        void publishEvent(const BoundaryEvent& event);

      Q_SIGNALS:
        void connectionOpened();
        void connectionClosed(SocketDisconnectReason reason, const QString& detail);

      private:
        struct PendingFrame
        {
            QByteArray data;
            SocketFrameKind kind = SocketFrameKind::ProtocolError;
            std::uint64_t correlation = 0;
            std::size_t offset = 0;
            bool coalescible = false;
            std::optional<BoundaryEvent> event;
        };

        void acceptConnection();
        void readSocket();
        void writeSocket(qint64 bytesWritten);
        void socketDisconnected();
        void socketError();
        void handleFrame(const SocketFrame& frame);
        void sendProtocolError(std::uint64_t correlation, const SocketFrameError& error);
        [[nodiscard]] bool enqueue(PendingFrame frame);
        [[nodiscard]] bool enqueueEvent(const BoundaryEvent& event);
        void pumpWrites();
        void disconnect(SocketDisconnectReason reason, QString detail);
        void clearSocket();
        [[nodiscard]] std::optional<SocketTransportError> validatePeer(QLocalSocket& socket) const;

        DaemonRequestHandler& m_handler;
        SocketEndpointOptions m_options;
        std::unique_ptr<QLocalServer> m_server;
        std::unique_ptr<QLocalSocket> m_socket;
        SocketFrameDecoder m_decoder;
        std::deque<PendingFrame> m_pendingWrites;
        std::optional<PendingFrame> m_currentWrite;
        std::size_t m_queuedBytes = 0;
        std::vector<ScopeId> m_activeScopes;
        bool m_handshakeComplete = false;
        bool m_processingFrame = false;
        bool m_inSocketCallback = false;
        bool m_disconnectScheduled = false;
        bool m_closeAfterWrites = false;
        SocketDisconnectReason m_closeReason = SocketDisconnectReason::None;
        QString m_closeDetail;
        std::optional<SocketTransportError> m_lastError;
    };

    class SocketActivationEndpoint final : public QObject
    {
        Q_OBJECT

      public:
        explicit SocketActivationEndpoint(DaemonRequestHandler& handler,
                                          SocketEndpointOptions options = {},
                                          QObject* parent = nullptr);
        ~SocketActivationEndpoint() override;

        SocketActivationEndpoint(const SocketActivationEndpoint&) = delete;
        SocketActivationEndpoint& operator=(const SocketActivationEndpoint&) = delete;
        SocketActivationEndpoint(SocketActivationEndpoint&&) = delete;
        SocketActivationEndpoint& operator=(SocketActivationEndpoint&&) = delete;

        [[nodiscard]] std::optional<SocketTransportError> listen();
        void close();
        [[nodiscard]] bool isListening() const;
        [[nodiscard]] const QString& socketPath() const;

      private:
        void acceptConnection();
        void readSocket();
        void socketDisconnected();
        void socketError();
        void clearSocket();
        [[nodiscard]] std::optional<SocketTransportError> validatePeer(QLocalSocket& socket) const;

        DaemonRequestHandler& m_handler;
        SocketEndpointOptions m_options;
        std::unique_ptr<QLocalServer> m_server;
        std::unique_ptr<QLocalSocket> m_socket;
        SocketFrameDecoder m_decoder;
        std::optional<SocketTransportError> m_lastError;
    };

    using SocketActivationResult = std::variant<std::optional<BoundaryError>, SocketTransportError>;

    class SocketActivationClient final
    {
      public:
        [[nodiscard]] static SocketActivationResult request(const SocketClientOptions& options,
                                                            ActivationRoute route);
    };

    class SocketDaemonClient final : public QObject,
                                     public CommandClient,
                                     public MaterializationClient,
                                     public SettingsClient,
                                     public DaemonStatusClient,
                                     public ActivationClient,
                                     public CacheAccessClient
    {
        Q_OBJECT

      public:
        explicit SocketDaemonClient(SocketClientOptions options = {}, QObject* parent = nullptr);
        ~SocketDaemonClient() override;

        SocketDaemonClient(const SocketDaemonClient&) = delete;
        SocketDaemonClient& operator=(const SocketDaemonClient&) = delete;
        SocketDaemonClient(SocketDaemonClient&&) = delete;
        SocketDaemonClient& operator=(SocketDaemonClient&&) = delete;

        [[nodiscard]] std::optional<SocketTransportError> connectToDaemon();
        void disconnectFromDaemon();
        [[nodiscard]] bool isConnected() const;
        [[nodiscard]] std::optional<SocketTransportError> lastError() const;

        [[nodiscard]] std::optional<BoundaryError> attachEventSink(BoundaryEventSink& sink);
        void detachEventSink(BoundaryEventSink& sink);

        [[nodiscard]] CommandReply submitCommand(CommandRequest request) override;
        [[nodiscard]] QFuture<CommandReply> submitCommandAsync(CommandRequest request);
        [[nodiscard]] MaterializationReply
        requestMaterialization(MaterializationRequest request) override;
        void cancelMaterializationScope(ScopeId scope) override;
        [[nodiscard]] SettingsReadReply getSettings() override;
        [[nodiscard]] SettingsUpdateReply updateSettings(UpdateSettingsRequest request) override;
        [[nodiscard]] HandshakeReply hello(HelloRequest request) override;
        [[nodiscard]] std::optional<BoundaryError> ping() override;
        [[nodiscard]] std::optional<BoundaryError> readyForActivation() override;
        [[nodiscard]] std::optional<BoundaryError> acknowledgeCacheAccessSuspended(
            CacheAccessSuspendedAcknowledgement acknowledgement) override;

      Q_SIGNALS:
        void connectionClosed(SocketDisconnectReason reason, const QString& detail);

      private:
        struct PendingFrame
        {
            QByteArray data;
            SocketFrameKind kind = SocketFrameKind::ProtocolError;
            std::uint64_t correlation = 0;
            std::size_t offset = 0;
        };

        struct ReceivedFrame
        {
            SocketFrameKind kind = SocketFrameKind::ProtocolError;
            QByteArray payload;
        };

        using AsyncFrameResult = std::variant<ReceivedFrame, SocketTransportError>;

        struct PendingAsyncReply
        {
            SocketFrameKind expectedKind = SocketFrameKind::ProtocolError;
            QPromise<AsyncFrameResult> promise;
        };

        [[nodiscard]] std::optional<SocketTransportError> ensureConnected();
        [[nodiscard]] std::optional<SocketTransportError> enqueue(PendingFrame frame);
        void readSocket();
        void writeSocket(qint64 bytesWritten);
        void socketDisconnected();
        void socketError();
        void handleFrame(const SocketFrame& frame);
        void pumpWrites();
        void clearSocket(SocketDisconnectReason reason, QString detail);
        [[nodiscard]] std::variant<ReceivedFrame, SocketTransportError>
        request(SocketFrameKind requestKind, const QByteArray& payload, SocketFrameKind replyKind);
        [[nodiscard]] QFuture<AsyncFrameResult> requestAsync(SocketFrameKind requestKind,
                                                             const QByteArray& payload,
                                                             SocketFrameKind replyKind);
        void timeoutAsyncReply(std::uint64_t correlation);
        [[nodiscard]] std::optional<SocketTransportError> waitForReply(std::uint64_t correlation,
                                                                       SocketFrameKind replyKind);
        [[nodiscard]] BoundaryError boundaryError(const SocketTransportError& error) const;
        [[nodiscard]] std::optional<SocketTransportError> validatePeer(QLocalSocket& socket) const;

        SocketClientOptions m_options;
        std::unique_ptr<QLocalSocket> m_socket;
        SocketFrameDecoder m_decoder;
        std::deque<PendingFrame> m_pendingWrites;
        std::optional<PendingFrame> m_currentWrite;
        std::map<std::uint64_t, ReceivedFrame> m_receivedReplies;
        std::map<std::uint64_t, std::unique_ptr<PendingAsyncReply>> m_asyncReplies;
        std::size_t m_queuedBytes = 0;
        std::size_t m_receivedBytes = 0;
        std::uint64_t m_nextCorrelation = 1;
        BoundaryEventSink* m_eventSink = nullptr;
        bool m_processingFrame = false;
        bool m_inSocketCallback = false;
        bool m_clearScheduled = false;
        std::optional<SocketTransportError> m_lastError;
    };

} // namespace javelin::protocol
