#pragma once

#include "protocol/ActivationContract.h"
#include "protocol/SocketFrameCodec.h"
#include "protocol/SocketSecurity.h"

#include <QObject>

#include <memory>
#include <optional>

class QLocalServer;
class QLocalSocket;

namespace javelin::protocol
{
    class SocketActivationEndpoint final : public QObject
    {
        Q_OBJECT

      public:
        explicit SocketActivationEndpoint(ActivationRequestHandler& handler,
                                          SocketEndpointOptions options = {},
                                          QObject* parent = nullptr);
        ~SocketActivationEndpoint() override;

        SocketActivationEndpoint(const SocketActivationEndpoint&) = delete;
        SocketActivationEndpoint& operator=(const SocketActivationEndpoint&) = delete;
        SocketActivationEndpoint(SocketActivationEndpoint&&) = delete;
        SocketActivationEndpoint& operator=(SocketActivationEndpoint&&) = delete;

        [[nodiscard]] std::optional<SocketTransportError> listen();
        void close();

      private:
        void acceptConnection();
        void readSocket();
        void socketDisconnected();
        void socketError();
        void finishSocket();
        void clearSocket();

        ActivationRequestHandler& m_handler;
        SocketEndpointOptions m_options;
        std::unique_ptr<QLocalServer> m_server;
        std::unique_ptr<QLocalSocket> m_socket;
        SocketFrameDecoder m_decoder;
        std::optional<SocketTransportError> m_lastError;
    };
} // namespace javelin::protocol
