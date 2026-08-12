#pragma once

#include "protocol/BoundaryEventContract.h"
#include "protocol/DaemonRequestHandler.h"
#include "protocol/SocketFrameCodec.h"
#include "protocol/SocketSecurity.h"

#include <QObject>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QLocalServer;
class QLocalSocket;

namespace javelin::protocol
{
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

        DaemonRequestHandler& m_handler;
        SocketEndpointOptions m_options;
        std::unique_ptr<QLocalServer> m_server;
        std::unique_ptr<QLocalSocket> m_socket;
        SocketFrameDecoder m_decoder;
        std::deque<PendingFrame> m_pendingWrites;
        std::unique_ptr<PendingFrame> m_currentWrite;
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
} // namespace javelin::protocol
