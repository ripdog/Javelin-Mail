#include "client/GuiDaemonSession.h"

#include "app/PerformanceMetrics.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <type_traits>
#include <utility>

#ifndef JAVELIN_INSTALL_BINDIR
#define JAVELIN_INSTALL_BINDIR ""
#endif

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

        [[nodiscard]] QString systemdUnitName()
        {
            return QStringLiteral("javelind.service");
        }

        struct SystemdCommandResult
        {
            int exitCode = -1;
            QString standardOutput;
            QString standardError;
        };

        [[nodiscard]] QString systemctlExecutable()
        {
            return QStandardPaths::findExecutable(QStringLiteral("systemctl"));
        }

        [[nodiscard]] std::optional<SystemdCommandResult>
        runSystemctl(const QStringList& arguments, const int timeoutMilliseconds)
        {
            const auto executable = systemctlExecutable();
            if (executable.isEmpty())
                return std::nullopt;

            QProcess process;
            process.setProgram(executable);
            process.setArguments(arguments);
            process.setProcessChannelMode(QProcess::SeparateChannels);
            process.start();
            if (!process.waitForStarted(timeoutMilliseconds))
                return std::nullopt;
            if (!process.waitForFinished(timeoutMilliseconds))
            {
                process.kill();
                process.waitForFinished();
                return std::nullopt;
            }

            return SystemdCommandResult{
                .exitCode = process.exitCode(),
                .standardOutput = QString::fromLocal8Bit(process.readAllStandardOutput()),
                .standardError = QString::fromLocal8Bit(process.readAllStandardError()),
            };
        }

        [[nodiscard]] bool runningFromConfiguredInstallDirectory()
        {
            constexpr auto configuredDirectory = JAVELIN_INSTALL_BINDIR;
            if constexpr (configuredDirectory[0] == '\0')
                return false;

            const auto applicationDirectory =
                QFileInfo{QCoreApplication::applicationFilePath()}.canonicalPath();
            const auto installDirectory =
                QFileInfo{QString::fromLatin1(configuredDirectory)}.canonicalFilePath();
            const auto systemDirectories =
                QStringList{QStringLiteral("/bin"),           QStringLiteral("/sbin"),
                            QStringLiteral("/usr/bin"),       QStringLiteral("/usr/sbin"),
                            QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/local/sbin")};
            return !applicationDirectory.isEmpty() && !installDirectory.isEmpty() &&
                   applicationDirectory == installDirectory &&
                   systemDirectories.contains(installDirectory);
        }

        [[nodiscard]] QString systemdFailureDetail(const QString& action,
                                                   const SystemdCommandResult& result)
        {
            const auto detail = result.standardError.trimmed();
            return detail.isEmpty()
                       ? QStringLiteral("systemctl --user %1 failed with exit status %2")
                             .arg(action)
                             .arg(result.exitCode)
                       : QStringLiteral("systemctl --user %1 failed: %2").arg(action, detail);
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
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("gui_startup")};
        if (m_readyReply.has_value())
        {
            metrics.finish(QStringLiteral("already_ready"));
            return std::nullopt;
        }
        if (const auto error = connectAndHandshake(true))
        {
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=handshake"));
            return error;
        }
        if (const auto error = loadSettingsAndCache())
        {
            m_client->disconnectFromDaemon();
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=cached_view"));
            return error;
        }
        if (const auto error = m_client->readyForActivation())
        {
            m_client->disconnectFromDaemon();
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=activation"));
            return detailError(GuiBootstrapErrorCode::DaemonUnavailable, error->detail);
        }
        m_inRecovery = false;
        Q_EMIT ready();
        metrics.finish(QStringLiteral("ready"), QStringLiteral("cached_view_ready=true"));
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::startDaemon()
    {
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("daemon_start")};
        if (isReady())
        {
            metrics.finish(QStringLiteral("already_ready"));
            return std::nullopt;
        }

        if (const auto error = launchDaemon())
        {
            metrics.finish(QStringLiteral("error"));
            return error;
        }

        const bool wasRecovering = m_inRecovery;
        const auto result = m_readyReply.has_value() ? reconnect() : start();
        if (!result.has_value() && wasRecovering && m_inRecovery)
        {
            m_inRecovery = false;
            Q_EMIT recoveryFinished();
        }
        metrics.finish(result.has_value() ? QStringLiteral("error") : QStringLiteral("ready"));
        return result;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::launchDaemon()
    {
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("daemon_launch")};
        if (!m_client->isConnected())
        {
            const auto executable = m_options.daemonExecutable.isEmpty()
                                        ? QDir{QCoreApplication::applicationDirPath()}.filePath(
                                              QStringLiteral("javelind"))
                                        : m_options.daemonExecutable;
            if (canUseSystemdUserService())
            {
                const auto result = runSystemctl(
                    {QStringLiteral("--user"), QStringLiteral("--no-pager"),
                     QStringLiteral("enable"), QStringLiteral("--now"), systemdUnitName()},
                    m_options.startTimeoutMilliseconds);
                if (!result.has_value())
                {
                    metrics.finish(QStringLiteral("error"),
                                   QStringLiteral("stage=enable_systemd_user_service"));
                    return detailError(
                        GuiBootstrapErrorCode::DaemonStartFailed,
                        QStringLiteral("could not run systemctl --user to start javelind"));
                }
                if (result->exitCode != 0)
                {
                    metrics.finish(QStringLiteral("error"),
                                   QStringLiteral("stage=enable_systemd_user_service"));
                    return detailError(
                        GuiBootstrapErrorCode::DaemonStartFailed,
                        systemdFailureDetail(QStringLiteral("enable --now javelind.service"),
                                             *result));
                }
            }
            else
            {
                QProcess daemonProcess;
                daemonProcess.setProgram(executable);
                daemonProcess.setArguments({QStringLiteral("--runtime-directory"),
                                            m_options.runtimeDirectory, QStringLiteral("--socket"),
                                            m_options.socketPath});
                const auto nullDevice = QProcess::nullDevice();
                daemonProcess.setStandardInputFile(nullDevice);
                if (qEnvironmentVariableIsSet("JAVELIN_FORWARD_DAEMON_STDIO"))
                {
                    // startDetached() does not forward process channels. Explicit device files keep
                    // development diagnostics attached to the terminal that launched `make run`.
                    daemonProcess.setStandardOutputFile(QStringLiteral("/dev/stdout"));
                    daemonProcess.setStandardErrorFile(QStringLiteral("/dev/stderr"));
                }
                else
                {
                    daemonProcess.setStandardOutputFile(nullDevice);
                    daemonProcess.setStandardErrorFile(nullDevice);
                }
                if (!daemonProcess.startDetached())
                {
                    metrics.finish(QStringLiteral("error"), QStringLiteral("stage=start_daemon"));
                    return detailError(
                        GuiBootstrapErrorCode::DaemonStartFailed,
                        QStringLiteral("could not start javelind: %1").arg(executable));
                }
            }

            QElapsedTimer timer;
            timer.start();
            while (!m_client->isConnected() && timer.elapsed() < m_options.startTimeoutMilliseconds)
            {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(20);
                static_cast<void>(m_client->connectToDaemon());
            }
            if (!m_client->isConnected())
            {
                metrics.finish(QStringLiteral("timeout"));
                return detailError(GuiBootstrapErrorCode::DaemonUnavailable,
                                   QStringLiteral("javelind did not become available"));
            }
        }
        metrics.finish(QStringLiteral("ready"));
        return std::nullopt;
    }

    bool GuiDaemonSession::canUseSystemdUserService() const
    {
        if (!runningFromConfiguredInstallDirectory())
            return false;

        const auto result = runSystemctl(
            {QStringLiteral("--user"), QStringLiteral("--no-pager"), QStringLiteral("show"),
             QStringLiteral("--property=LoadState"), QStringLiteral("--value"), systemdUnitName()},
            2000);
        return result.has_value() && result->exitCode == 0 &&
               result->standardOutput.trimmed() == QStringLiteral("loaded");
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::reconnect()
    {
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("gui_reconnect")};
        const auto oldReady = m_readyReply;
        if (const auto error = connectAndHandshake(false))
        {
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=handshake"));
            return error;
        }
        if (const auto error = loadSettingsAndCache())
        {
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=cached_view"));
            return error;
        }
        if (const auto error = m_client->readyForActivation())
        {
            m_client->disconnectFromDaemon();
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=activation"));
            return detailError(GuiBootstrapErrorCode::DaemonUnavailable, error->detail);
        }

        if (oldReady.has_value() && (oldReady->cache.instance != m_readyReply->cache.instance ||
                                     oldReady->cache.schema != m_readyReply->cache.schema))
        {
            if (const auto error = suspendReadAccess())
            {
                metrics.finish(QStringLiteral("error"), QStringLiteral("stage=suspend_cache"));
                return error;
            }
            if (const auto error = resumeReadAccess())
            {
                metrics.finish(QStringLiteral("error"), QStringLiteral("stage=resume_cache"));
                return error;
            }
        }
        m_inRecovery = false;
        Q_EMIT recoveryFinished();
        metrics.finish(QStringLiteral("ready"));
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
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("command_admission"),
                                QStringLiteral("kind=refresh_account")};
        const auto reply = m_client->submitCommand(
            {.id = protocol::CommandId{.value = QUuid::createUuid()},
             .command = protocol::RefreshAccountCommand{.accountId = accountId, .force = true}});
        if (const auto* rejected = std::get_if<protocol::CommandRejected>(&reply))
        {
            metrics.finish(QStringLiteral("rejected"));
            return rejected->error;
        }
        metrics.finish(QStringLiteral("accepted"));
        return std::nullopt;
    }

    protocol::CommandReply GuiDaemonSession::submitRemoteAction(const protocol::ActionId action,
                                                                QByteArray payload,
                                                                const protocol::CommandId id)
    {
        return m_client->submitCommand({
            .id = id,
            .command =
                protocol::RemoteActionCommand{.action = action, .payload = std::move(payload)},
        });
    }

    QFuture<protocol::CommandReply>
    GuiDaemonSession::submitRemoteActionAsync(const protocol::ActionId action, QByteArray payload,
                                              const protocol::CommandId id)
    {
        return m_client->submitCommandAsync({
            .id = id,
            .command =
                protocol::RemoteActionCommand{.action = action, .payload = std::move(payload)},
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
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("materialization_admission"),
                                QStringLiteral("offset=%1 limit=%2").arg(offset).arg(limit)};
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
        {
            metrics.finish(QStringLiteral("rejected"));
            return rejected->error;
        }
        m_materializationScope = scope;
        metrics.finish(QStringLiteral("accepted"));
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
                else if constexpr (std::is_same_v<Event, protocol::ThreadMaterializationProgress>)
                {
                    Q_EMIT threadMaterializationProgress(value);
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
                else if constexpr (std::is_same_v<Event, protocol::DaemonLogEntries>)
                {
                    for (const auto& entry : value.entries)
                    {
                        Q_EMIT daemonLogEntryAdded({
                            .timestamp = QDateTime::fromMSecsSinceEpoch(
                                static_cast<qint64>(entry.timestampMilliseconds)),
                            .level = static_cast<QtMsgType>(entry.level),
                            .subsystem = entry.subsystem,
                            .message = entry.message,
                        });
                    }
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
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("daemon_handshake"),
                                QStringLiteral("allow_start=%1").arg(allowStart)};
        if (!m_client->isConnected())
        {
            if (const auto error = m_client->connectToDaemon())
            {
                if (!allowStart || !m_options.startDaemonIfMissing)
                {
                    metrics.finish(QStringLiteral("unavailable"));
                    return transportError(*error);
                }

                if (const auto startError = launchDaemon())
                    return startError;
            }
        }

        const auto handshake =
            m_client->hello({.protocol = m_options.protocol, .build = m_options.build});
        if (const auto* rejected = std::get_if<protocol::HandshakeRejected>(&handshake))
        {
            m_client->disconnectFromDaemon();
            metrics.finish(QStringLiteral("rejected"));
            const auto code = rejected->error.code == protocol::BoundaryErrorCode::IncompatibleBuild
                                  ? GuiBootstrapErrorCode::IncompatibleDaemon
                                  : GuiBootstrapErrorCode::DaemonUnavailable;
            return detailError(code, rejected->error.detail);
        }
        m_readyReply = std::get<protocol::ReadyReply>(handshake);
        m_currentEpoch = m_readyReply->epoch.value;
        metrics.finish(QStringLiteral("ready"));
        return std::nullopt;
    }

    std::optional<GuiBootstrapError> GuiDaemonSession::refreshSettings()
    {
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("settings_read")};
        const auto reply = m_client->getSettings();
        if (const auto* rejected = std::get_if<protocol::SettingsReadRejected>(&reply))
        {
            metrics.finish(QStringLiteral("rejected"));
            return detailError(GuiBootstrapErrorCode::SettingsUnavailable, rejected->error.detail);
        }
        m_settings = std::get<protocol::SettingsSnapshotReply>(reply).snapshot;
        metrics.finish(QStringLiteral("ready"));
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
        PerformanceSpan metrics{QStringLiteral("gui"), QStringLiteral("cache_read_connection")};
        if (m_databasePath.isEmpty())
        {
            metrics.finish(QStringLiteral("error"), QStringLiteral("stage=path"));
            return detailError(GuiBootstrapErrorCode::CacheUnavailable,
                               QStringLiteral("daemon did not provide a cache path"));
        }
        if (readConnectionOpen())
        {
            metrics.finish(QStringLiteral("already_open"));
            return std::nullopt;
        }
        auto opened = javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
                .databasePath = m_databasePath,
            }}.openForCurrentThread("bootstrap");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
        {
            metrics.finish(QStringLiteral("error"));
            return detailError(GuiBootstrapErrorCode::CacheUnavailable, error->message);
        }
        m_readConnection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));
        metrics.finish(QStringLiteral("ready"));
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
