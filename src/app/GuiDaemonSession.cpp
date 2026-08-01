#include "app/GuiDaemonSession.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] GuiBootstrapError transportError(const protocol::SocketTransportError& error)
        {
            return {.code = GuiBootstrapErrorCode::DaemonUnavailable, .detail = error.detail};
        }

        [[nodiscard]] GuiBootstrapError detailError(const GuiBootstrapErrorCode code,
                                                    QString detail)
        {
            return {.code = code, .detail = std::move(detail)};
        }
    } // namespace

    GuiDaemonSession::GuiDaemonSession(GuiDaemonSessionOptions options, QObject* parent)
        : QObject(parent), m_options(std::move(options))
    {
        protocol::SocketClientOptions socketOptions{
            .runtimeDirectory = m_options.runtimeDirectory,
            .socketPath = m_options.socketPath,
            .limits = {.maximumStringBytes = 4096,
                       .maximumCollectionItems = 256,
                       .maximumAffectedKeys = 64,
                       .maximumMaterializationItems = 500,
                       .maximumFrameBytes = 64 * 1024 * 1024},
            .protocol = m_options.protocol,
            .expectedBuild = m_options.build,
            .maximumQueuedFrames = 128,
            .maximumQueuedBytes = 128 * 1024 * 1024,
            .responseTimeoutMilliseconds = 5000,
            .enforcePeerCredentials = true,
        };
        m_client = std::make_unique<protocol::SocketDaemonClient>(std::move(socketOptions), this);
        static_cast<void>(m_client->attachEventSink(*this));
        connect(m_client.get(), &protocol::SocketDaemonClient::connectionClosed, this,
                &GuiDaemonSession::onDaemonDisconnected);

        m_cacheParticipant = m_cacheAccessBarrier.registerParticipant({
            .name = QStringLiteral("GUI read connections"),
            .suspend = [this]() -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                m_readConnection = javelin::jmap::cache::ReadOnlyDatabaseConnection{};
                return std::nullopt;
            },
            .resume = [this]() -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                if (const auto error = openReadConnection())
                    return javelin::jmap::cache::DatabaseError{
                        .code = javelin::jmap::cache::DatabaseErrorCode::OpenFailed,
                        .message = error->detail};
                return std::nullopt;
            },
        });
    }

    GuiDaemonSession::~GuiDaemonSession()
    {
        stop();
        if (m_cacheParticipant != 0)
            m_cacheAccessBarrier.unregisterParticipant(m_cacheParticipant);
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::start()
    {
        if (m_readyReply.has_value())
            return std::nullopt;
        if (const auto error = connectAndHandshake(true))
            return error;
        if (const auto error = loadSettingsAndCache())
        {
            m_client->disconnectFromDaemon();
            return error;
        }
        if (const auto error = m_client->readyForActivation())
        {
            m_client->disconnectFromDaemon();
            return detailError(GuiBootstrapErrorCode::DaemonUnavailable, error->detail);
        }
        m_inRecovery = false;
        Q_EMIT ready();
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::reconnect()
    {
        const auto oldReady = m_readyReply;
        if (const auto error = connectAndHandshake(false))
            return error;
        if (const auto error = loadSettingsAndCache())
            return error;

        if (oldReady.has_value() && (oldReady->cache.instance != m_readyReply->cache.instance ||
                                     oldReady->cache.schema != m_readyReply->cache.schema))
        {
            if (const auto error = suspendReadAccess())
                return error;
            if (const auto error = resumeReadAccess())
                return error;
        }
        m_inRecovery = false;
        Q_EMIT recoveryFinished();
        return std::nullopt;
    }

    void GuiDaemonSession::stop()
    {
        cancelMaterializationScope();
        if (m_client != nullptr)
            m_client->disconnectFromDaemon();
        m_readConnection = javelin::jmap::cache::ReadOnlyDatabaseConnection{};
        m_readyReply.reset();
        m_inRecovery = false;
    }

    bool GuiDaemonSession::isReady() const
    {
        return m_readyReply.has_value() && !m_inRecovery && m_client->isConnected();
    }

    const protocol::SettingsSnapshot& GuiDaemonSession::settings() const
    {
        return m_settings;
    }

    const std::optional<protocol::DaemonStatus>& GuiDaemonSession::daemonStatus() const
    {
        return m_daemonStatus;
    }

    std::optional<protocol::DaemonInstanceId> GuiDaemonSession::daemonInstance() const
    {
        if (!m_readyReply.has_value())
            return std::nullopt;
        return m_readyReply->daemon;
    }

    std::optional<protocol::BoundaryError>
    GuiDaemonSession::updateSettings(const protocol::SettingsRevision baseRevision,
                                     protocol::SettingsUpdate update)
    {
        const auto reply =
            m_client->updateSettings({.baseRevision = baseRevision, .update = std::move(update)});
        if (const auto* rejected = std::get_if<protocol::SettingsUpdateRejected>(&reply))
            return rejected->error;

        if (const auto error = refreshSettings())
        {
            return protocol::BoundaryError{
                .code = protocol::BoundaryErrorCode::SettingsStorageFailure,
                .field = QStringLiteral("settings"),
                .detail = error->detail,
            };
        }
        Q_EMIT settingsChanged();
        return std::nullopt;
    }

    QMetaObject::Connection GuiDaemonSession::connectSettingsChanged(QObject* context,
                                                                     std::function<void()> callback)
    {
        return connect(this, &GuiDaemonSession::settingsChanged, context, std::move(callback));
    }

    std::optional<protocol::BoundaryError>
    GuiDaemonSession::requestAccountRefresh(const QString& accountId)
    {
        const auto reply = m_client->submitCommand(
            {.id = protocol::CommandId{.value = QUuid::createUuid()},
             .command = protocol::RefreshAccountCommand{.accountId = accountId, .force = true}});
        if (const auto* rejected = std::get_if<protocol::CommandRejected>(&reply))
            return rejected->error;
        return std::nullopt;
    }

    protocol::CommandReply
    GuiDaemonSession::submitRemoteAction(const protocol::RemoteActionKind kind, QByteArray payload,
                                         const protocol::CommandId id)
    {
        return m_client->submitCommand({
            .id = id,
            .command = protocol::RemoteActionCommand{.kind = kind, .payload = std::move(payload)},
        });
    }

    QFuture<protocol::CommandReply>
    GuiDaemonSession::submitRemoteActionAsync(const protocol::RemoteActionKind kind,
                                              QByteArray payload, const protocol::CommandId id)
    {
        return m_client->submitCommandAsync({
            .id = id,
            .command = protocol::RemoteActionCommand{.kind = kind, .payload = std::move(payload)},
        });
    }

    CacheAccessBarrier::ParticipantId
    GuiDaemonSession::registerCacheParticipant(CacheAccessBarrier::Participant participant)
    {
        return m_cacheAccessBarrier.registerParticipant(std::move(participant));
    }

    void GuiDaemonSession::unregisterCacheParticipant(
        const CacheAccessBarrier::ParticipantId participant)
    {
        m_cacheAccessBarrier.unregisterParticipant(participant);
    }

    std::optional<protocol::BoundaryError>
    GuiDaemonSession::requestMailboxWindow(const QString& accountId, const QString& mailboxId,
                                           const std::uint64_t offset, const std::uint32_t limit)
    {
        cancelMaterializationScope();
        const auto scope = protocol::ScopeId{.value = QUuid::createUuid()};
        const auto reply = m_client->requestMaterialization(
            {.id = protocol::RequestId{.value = QUuid::createUuid()},
             .scope = scope,
             .request = protocol::MailboxWindowMaterialization{.accountId = accountId,
                                                               .mailboxId = mailboxId,
                                                               .offset = offset,
                                                               .limit = limit}});
        if (const auto* rejected = std::get_if<protocol::MaterializationRejected>(&reply))
            return rejected->error;
        m_materializationScope = scope;
        return std::nullopt;
    }

    const QString& GuiDaemonSession::databasePath() const
    {
        return m_databasePath;
    }

    bool GuiDaemonSession::readConnectionOpen() const
    {
        return !m_readConnection.connectionName().isEmpty();
    }

    void GuiDaemonSession::onBoundaryEvent(const protocol::BoundaryEvent& event)
    {
        std::visit(
            [this](const auto& value)
            {
                using Event = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Event, protocol::CacheInvalidation>)
                {
                    m_currentEpoch = std::max(m_currentEpoch, value.epoch.value);
                    Q_EMIT cacheInvalidated(value);
                    Q_EMIT cacheChanged();
                }
                else if constexpr (std::is_same_v<Event, protocol::OperationCompleted>)
                {
                    Q_EMIT operationCompleted(value.operation, value.result);
                }
                else if constexpr (std::is_same_v<Event, protocol::OperationFailed>)
                {
                    Q_EMIT operationFailed(value.operation, value.error);
                }
                else if constexpr (std::is_same_v<Event, protocol::CacheAccessSuspendRequested>)
                {
                    QTimer::singleShot(0, this, [this, request = value]
                                       { acknowledgeCacheSuspend(request); });
                }
                else if constexpr (std::is_same_v<Event, protocol::CacheAccessResumed>)
                {
                    m_currentEpoch = std::max(m_currentEpoch, value.epoch.value);
                    m_databasePath = value.cacheDatabasePath;
                    if (const auto error = resumeReadAccess())
                    {
                        beginRecovery(error->detail);
                    }
                    else
                    {
                        if (m_readyReply.has_value())
                        {
                            m_readyReply->cache = value.cache;
                            m_readyReply->cacheDatabasePath = value.cacheDatabasePath;
                        }
                        Q_EMIT cacheChanged();
                    }
                }
                else if constexpr (std::is_same_v<Event, protocol::ActivationRequested>)
                {
                    Q_EMIT activationRequested(value.route);
                }
                else if constexpr (std::is_same_v<Event, protocol::DaemonStatusChanged>)
                {
                    m_daemonStatus = value.status;
                    Q_EMIT daemonStatusChanged(value.status);
                }
                else if constexpr (std::is_same_v<Event, protocol::SettingsUpdated>)
                {
                    if (!refreshSettings().has_value())
                        Q_EMIT settingsChanged();
                }
                else if constexpr (std::is_same_v<Event, protocol::DaemonShutdownRequested>)
                {
                    Q_EMIT daemonShutdownRequested();
                }
            },
            event);
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::connectAndHandshake(const bool allowStart)
    {
        if (!m_client->isConnected())
        {
            if (const auto error = m_client->connectToDaemon())
            {
                if (!allowStart || !m_options.startDaemonIfMissing)
                    return transportError(*error);

                const auto executable = m_options.daemonExecutable.isEmpty()
                                            ? QDir{QCoreApplication::applicationDirPath()}.filePath(
                                                  QStringLiteral("javelind"))
                                            : m_options.daemonExecutable;
                const QStringList arguments{QStringLiteral("--runtime-directory"),
                                            m_options.runtimeDirectory, QStringLiteral("--socket"),
                                            m_options.socketPath};
                if (!QProcess::startDetached(executable, arguments))
                    return detailError(
                        GuiBootstrapErrorCode::DaemonStartFailed,
                        QStringLiteral("could not start javelind: %1").arg(executable));

                QElapsedTimer timer;
                timer.start();
                while (!m_client->isConnected() &&
                       timer.elapsed() < m_options.startTimeoutMilliseconds)
                {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(20);
                    if (m_client->connectToDaemon().has_value())
                        continue;
                }
                if (!m_client->isConnected())
                    return detailError(GuiBootstrapErrorCode::DaemonUnavailable,
                                       QStringLiteral("javelind did not become available"));
            }
        }

        const auto handshake =
            m_client->hello({.protocol = m_options.protocol, .build = m_options.build});
        if (const auto* rejected = std::get_if<protocol::HandshakeRejected>(&handshake))
        {
            m_client->disconnectFromDaemon();
            const auto code = rejected->error.code == protocol::BoundaryErrorCode::IncompatibleBuild
                                  ? GuiBootstrapErrorCode::IncompatibleDaemon
                                  : GuiBootstrapErrorCode::DaemonUnavailable;
            return detailError(code, rejected->error.detail);
        }
        m_readyReply = std::get<protocol::ReadyReply>(handshake);
        m_currentEpoch = m_readyReply->epoch.value;
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::refreshSettings()
    {
        const auto reply = m_client->getSettings();
        if (const auto* rejected = std::get_if<protocol::SettingsReadRejected>(&reply))
            return detailError(GuiBootstrapErrorCode::SettingsUnavailable, rejected->error.detail);
        m_settings = std::get<protocol::SettingsSnapshotReply>(reply).snapshot;
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::loadSettingsAndCache()
    {
        if (const auto error = refreshSettings())
            return error;

        if (!m_readyReply.has_value() || m_readyReply->cacheDatabasePath.isEmpty())
            return detailError(GuiBootstrapErrorCode::CacheUnavailable,
                               QStringLiteral("daemon did not provide a cache path"));
        m_databasePath = m_readyReply->cacheDatabasePath;
        return openReadConnection();
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::openReadConnection()
    {
        if (m_databasePath.isEmpty())
            return detailError(GuiBootstrapErrorCode::CacheUnavailable,
                               QStringLiteral("daemon did not provide a cache path"));
        if (readConnectionOpen())
            return std::nullopt;
        auto opened = javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
                .databasePath = m_databasePath,
            }}.openForCurrentThread("bootstrap");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            return detailError(GuiBootstrapErrorCode::CacheUnavailable, error->message);
        m_readConnection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::suspendReadAccess()
    {
        if (const auto error = m_cacheAccessBarrier.suspend())
            return detailError(GuiBootstrapErrorCode::CacheBarrierFailed, error->message);
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::resumeReadAccess()
    {
        if (const auto error = m_cacheAccessBarrier.resume())
            return detailError(GuiBootstrapErrorCode::CacheBarrierFailed, error->message);
        return std::nullopt;
    }

    void GuiDaemonSession::cancelMaterializationScope()
    {
        if (!m_materializationScope.has_value())
            return;
        if (m_client != nullptr && m_client->isConnected())
            m_client->cancelMaterializationScope(*m_materializationScope);
        m_materializationScope.reset();
    }

    void GuiDaemonSession::beginRecovery(const QString& detail)
    {
        m_inRecovery = true;
        Q_EMIT recoveryStarted(detail);
    }

    void GuiDaemonSession::onDaemonDisconnected(const protocol::SocketDisconnectReason,
                                                const QString& detail)
    {
        if (m_inRecovery)
            return;
        beginRecovery(detail);
    }

    void GuiDaemonSession::acknowledgeCacheSuspend(protocol::CacheAccessSuspendRequested request)
    {
        if (const auto error = suspendReadAccess())
        {
            beginRecovery(error->detail);
            return;
        }
        if (const auto error = m_client->acknowledgeCacheAccessSuspended({
                .instance = request.instance,
            }))
        {
            beginRecovery(error->detail);
        }
    }
} // namespace javelin::app
