#include "protocol/SocketWireCodecInternal.h"

#include <QAbstractSocket>
#include <QDir>
#include <QFileDevice>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>

#ifdef Q_OS_LINUX
#include <sys/socket.h>
#include <unistd.h>
#elif defined(Q_OS_MACOS)
#include <unistd.h>
#endif

#include <utility>

namespace javelin::protocol::detail
{
    std::optional<SocketTransportError>
    validateRuntimeDirectory(const SocketEndpointOptions& options)
    {
        if (options.runtimeDirectory.isEmpty() || options.socketPath.isEmpty())
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::TransportFailure,
                .detail = QStringLiteral("runtime directory and socket path are required")};
        }
        const QFileInfo directoryInfo{options.runtimeDirectory};
        if (!directoryInfo.exists() || !directoryInfo.isDir())
        {
            return SocketTransportError{.reason = SocketDisconnectReason::TransportFailure,
                                        .detail =
                                            QStringLiteral("runtime directory does not exist")};
        }
        const auto permissions = directoryInfo.permissions();
        if (directoryInfo.ownerId() != static_cast<uint>(::geteuid()) ||
            !(permissions & QFileDevice::ReadOwner) || !(permissions & QFileDevice::WriteOwner) ||
            !(permissions & QFileDevice::ExeOwner) ||
            (permissions &
             (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
              QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)))
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::CredentialFailure,
                .detail = QStringLiteral("runtime directory must be a private directory")};
        }
        const QString runtime = QDir::cleanPath(options.runtimeDirectory);
        const QString socket = QDir::cleanPath(options.socketPath);
        const QString relative = QDir{runtime}.relativeFilePath(socket);
        if (relative.isEmpty() || relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::CredentialFailure,
                .detail = QStringLiteral("socket path must be inside the runtime directory")};
        }
        return std::nullopt;
    }

    std::optional<SocketTransportError> listenOnLocalServer(QLocalServer& server,
                                                            const SocketEndpointOptions& options,
                                                            QString addressInUseDetail)
    {
        if (const auto error = validateRuntimeDirectory(options))
            return error;
        if (server.isListening())
            return std::nullopt;
        if (server.listen(options.socketPath))
            return std::nullopt;
        if (server.serverError() != QAbstractSocket::AddressInUseError)
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::TransportFailure,
                .detail = server.errorString(),
            };
        }

        QLocalSocket probe;
        probe.connectToServer(options.socketPath);
        if (probe.waitForConnected(100))
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::TransportFailure,
                .detail = std::move(addressInUseDetail),
            };
        }

        QLocalServer::removeServer(options.socketPath);
        if (server.listen(options.socketPath))
            return std::nullopt;
        return SocketTransportError{
            .reason = SocketDisconnectReason::TransportFailure,
            .detail = server.errorString(),
        };
    }

    void closeLocalServer(QLocalServer* server, const QString& socketPath)
    {
        if (server == nullptr)
            return;
        const bool wasListening = server->isListening();
        server->close();
        if (wasListening && !socketPath.isEmpty())
            QLocalServer::removeServer(socketPath);
    }

    std::optional<SocketTransportError> validatePeerCredentials(QLocalSocket& socket,
                                                                const bool enforce)
    {
        if (!enforce)
            return std::nullopt;
#ifdef Q_OS_LINUX
        struct ucred peer;
        socklen_t length = sizeof(peer);
        const int descriptor = static_cast<int>(socket.socketDescriptor());
        if (descriptor < 0 || getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0)
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::CredentialFailure,
                .detail = QStringLiteral("could not inspect local socket credentials")};
        }
        if (peer.uid != static_cast<uid_t>(::geteuid()))
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::CredentialFailure,
                .detail = QStringLiteral("local socket peer has a different user id")};
        }
#elif defined(Q_OS_MACOS)
        uid_t uid = 0;
        gid_t gid = 0;
        if (getpeereid(socket.socketDescriptor(), &uid, &gid) != 0 ||
            uid != static_cast<uid_t>(::geteuid()))
        {
            return SocketTransportError{
                .reason = SocketDisconnectReason::CredentialFailure,
                .detail = QStringLiteral("local socket peer credentials are invalid")};
        }
#else
        Q_UNUSED(socket);
#endif
        return std::nullopt;
    }
} // namespace javelin::protocol::detail
