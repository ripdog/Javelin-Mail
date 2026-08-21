#pragma once

#include "protocol/BoundaryEventContract.h"
#include "protocol/CacheContract.h"
#include "protocol/HandshakeContract.h"
#include "protocol/SettingsContract.h"
#include "protocol/SocketFrameCodec.h"
#include "protocol/SocketSecurity.h"

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

class QLocalSocket;

namespace javelin::protocol
{
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
        void deferredReplyAvailable(SocketFrameKind requestKind);

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

        struct DeferredReply
        {
            SocketFrameKind requestKind = SocketFrameKind::ProtocolError;
            SocketFrameKind expectedKind = SocketFrameKind::ProtocolError;
            QByteArray requestPayload;
            std::optional<ReceivedFrame> received;
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
        [[nodiscard]] std::optional<SocketTransportError>
        waitForReply(std::uint64_t correlation, SocketFrameKind requestKind,
                     const QByteArray& requestPayload, SocketFrameKind replyKind);
        [[nodiscard]] BoundaryError boundaryError(const SocketTransportError& error) const;

        SocketClientOptions m_options;
        std::unique_ptr<QLocalSocket> m_socket;
        SocketFrameDecoder m_decoder;
        std::deque<PendingFrame> m_pendingWrites;
        std::unique_ptr<PendingFrame> m_currentWrite;
        std::map<std::uint64_t, ReceivedFrame> m_receivedReplies;
        std::map<std::uint64_t, std::unique_ptr<PendingAsyncReply>> m_asyncReplies;
        std::map<std::uint64_t, DeferredReply> m_abandonedReplies;
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
