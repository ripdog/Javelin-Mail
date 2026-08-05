#include "app/DaemonProcess.h"

#include "app/CacheAccessBarrier.h"
#include "app/CalendarNotificationService.h"
#include "app/CommandDispatcher.h"
#include "app/DaemonBackgroundController.h"
#include "app/DaemonRemoteActionDispatcher.h"
#include "app/DaemonServices.h"
#include "app/DeferredSendService.h"
#include "app/FullMailSyncService.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MailApplicationService.h"
#include "app/MailIndexService.h"
#include "app/PerformanceMetrics.h"
#include "app/ProcessInstanceLock.h"
#include "app/SettingsRepository.h"
#include "app/UndoApplicationPorts.h"
#include "app/WorkScheduler.h"
#include "app/undo/UndoManager.h"

#include "jmap/auth/AccountOnboardingService.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMetaObject>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <exception>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace javelin::app
{
    namespace
    {
        using namespace javelin::protocol;

        [[nodiscard]] AccountConnectionSettings connectionSettings(const AccountSettings& settings)
        {
            return {.connectionId = settings.id.toStdString(),
                    .revision = settings.revision,
                    .sessionUrl = settings.sessionUrl.toStdString(),
                    .loginEmail = settings.loginEmail.toStdString(),
                    .apiKey = settings.apiKey.toStdString(),
                    .refreshToken = settings.refreshToken.toStdString(),
                    .tokenEndpoint = settings.tokenEndpoint.toStdString(),
                    .oauthClientId = settings.oauthClientId.toStdString(),
                    .oauthIssuer = settings.oauthIssuer.toStdString(),
                    .oauthResource = settings.oauthResource.toStdString(),
                    .oauthScope = settings.oauthScope.toStdString(),
                    .revocationEndpoint = settings.revocationEndpoint.toStdString()};
        }

        [[nodiscard]] const MailboxSelectionSettings*
        findSelection(const std::vector<MailboxSelectionSettings>& selections,
                      const QString& accountId)
        {
            const auto found =
                std::ranges::find(selections, accountId, &MailboxSelectionSettings::accountId);
            return found == selections.end() ? nullptr : &*found;
        }

        [[nodiscard]] std::vector<std::string> stringIds(const std::vector<QString>& ids)
        {
            std::vector<std::string> result;
            result.reserve(ids.size());
            for (const auto& id : ids)
                result.push_back(id.toStdString());
            return result;
        }

        [[nodiscard]] std::vector<AccountSyncConfiguration>
        accountConfigurations(const SettingsSnapshot& snapshot)
        {
            std::vector<AccountSyncConfiguration> result;
            for (const auto& account : snapshot.accounts)
            {
                if (account.loginEmail.isEmpty() || account.apiKey.isEmpty())
                    continue;

                const auto appendConfiguration = [&](const QString& accountId)
                {
                    const auto* synced = findSelection(snapshot.syncedMailboxSelections, accountId);
                    const auto* notifications =
                        findSelection(snapshot.notificationMailboxSelections, accountId);
                    std::vector<QString> mailboxIds;
                    if (synced != nullptr)
                        mailboxIds = synced->mailboxIds;
                    if (notifications != nullptr)
                    {
                        mailboxIds.insert(mailboxIds.end(), notifications->mailboxIds.begin(),
                                          notifications->mailboxIds.end());
                    }
                    std::ranges::sort(mailboxIds);
                    mailboxIds.erase(std::ranges::unique(mailboxIds).begin(), mailboxIds.end());

                    result.push_back({
                        .settings = connectionSettings(account),
                        .accountId = accountId.toStdString(),
                        .mailboxIds = stringIds(mailboxIds),
                        .fullSyncMailboxIds = synced == nullptr ? std::vector<std::string>{}
                                                                : stringIds(synced->mailboxIds),
                        .notificationMailboxIds = notifications == nullptr
                                                      ? std::vector<std::string>{}
                                                      : stringIds(notifications->mailboxIds),
                    });
                };
                for (const auto& accountId : account.cachedAccountIds)
                    appendConfiguration(accountId);
                if (account.cachedAccountIds.empty())
                    appendConfiguration(account.id);
            }
            return result;
        }

        [[nodiscard]] BoundaryError settingsError(const SettingsRepositoryError& error)
        {
            BoundaryErrorCode code = BoundaryErrorCode::SettingsStorageFailure;
            if (error.code == SettingsRepositoryErrorCode::MigrationFailed ||
                error.code == SettingsRepositoryErrorCode::UnsupportedSchema)
                code = BoundaryErrorCode::SettingsMigrationFailure;
            return BoundaryError{.code = code, .field = error.key, .detail = error.detail};
        }

        [[nodiscard]] AccountState accountState(const MailAccountStatus status)
        {
            switch (status)
            {
            case MailAccountStatus::Disconnected:
                return AccountState::Paused;
            case MailAccountStatus::Connecting:
                return AccountState::Synchronizing;
            case MailAccountStatus::Connected:
                return AccountState::Ready;
            case MailAccountStatus::AuthenticationPaused:
                return AccountState::AuthenticationRequired;
            }
            return AccountState::Unknown;
        }

        [[nodiscard]] QString startupDetail(const std::exception& exception)
        {
            return QString::fromUtf8(exception.what());
        }

        [[nodiscard]] bool daemonSocketIsAlive(const QString& socketPath)
        {
            QElapsedTimer timer;
            timer.start();
            constexpr qint64 startupGraceMilliseconds = 1000;
            do
            {
                QLocalSocket socket;
                socket.connectToServer(socketPath, QIODevice::ReadWrite);
                if (socket.waitForConnected(50))
                {
                    socket.disconnectFromServer();
                    return true;
                }
                QThread::msleep(25);
            } while (timer.elapsed() < startupGraceMilliseconds);
            return false;
        }
    } // namespace

    DaemonProcess::DaemonProcess(DaemonProcessOptions options, QObject* parent)
        : QObject(parent), m_options(std::move(options))
    {
        if (!m_options.socket.expectedBuild.has_value())
            m_options.socket.expectedBuild = m_options.build;
        m_options.socket.protocol = m_options.protocol;
        m_performanceTimer.setInterval(30'000);
        connect(&m_performanceTimer, &QTimer::timeout, this, &DaemonProcess::samplePerformance);
        m_oauthRefreshTimer.setInterval(60'000);
        connect(&m_oauthRefreshTimer, &QTimer::timeout, this,
                &DaemonProcess::refreshOAuthCredentials);
    }

    DaemonProcess::~DaemonProcess()
    {
        stop();
    }

    std::optional<DaemonStartupError> DaemonProcess::start()
    {
        if (m_lifecycle == DaemonLifecycle::Ready)
            return std::nullopt;
        if (m_lifecycle == DaemonLifecycle::ShuttingDown)
            return fail(DaemonStartupErrorCode::SocketListen,
                        QStringLiteral("daemon has already been stopped"));

        if (m_options.socket.runtimeDirectory.isEmpty())
            return fail(DaemonStartupErrorCode::SocketListen,
                        QStringLiteral("runtime directory is required"));

        m_instanceLock = std::make_unique<QLockFile>(
            QDir{m_options.socket.runtimeDirectory}.filePath(QStringLiteral("javelind.lock")));
        m_instanceLock->setStaleLockTime(0);
        if (!m_instanceLock->tryLock(0))
        {
            if (m_instanceLock->error() != QLockFile::LockFailedError)
                return fail(DaemonStartupErrorCode::SocketListen,
                            QStringLiteral("could not acquire daemon instance lock: %1")
                                .arg(static_cast<int>(m_instanceLock->error())));

            if (!daemonSocketIsAlive(m_options.socket.socketPath))
            {
                const auto recovery =
                    recoverAbandonedProcessLock(*m_instanceLock, QStringLiteral("javelind"));
                if (recovery == ProcessLockRecoveryResult::RemovalFailed)
                {
                    return fail(DaemonStartupErrorCode::SocketListen,
                                QStringLiteral("could not remove stale daemon instance lock"));
                }
                if (recovery == ProcessLockRecoveryResult::Removed && m_instanceLock->tryLock(0))
                {
                    qInfo() << QStringLiteral("Removed stale daemon instance lock");
                }
                else if (recovery == ProcessLockRecoveryResult::Removed &&
                         m_instanceLock->error() != QLockFile::LockFailedError)
                {
                    return fail(DaemonStartupErrorCode::SocketListen,
                                QStringLiteral("could not acquire daemon instance lock: %1")
                                    .arg(static_cast<int>(m_instanceLock->error())));
                }
            }

            if (!m_instanceLock->isLocked())
            {
                qint64 pid = 0;
                QString hostname;
                QString applicationName;
                auto detail = QStringLiteral("another javelind instance is already running");
                if (m_instanceLock->getLockInfo(&pid, &hostname, &applicationName))
                    detail += QStringLiteral(" (pid %1)").arg(pid);
                return fail(DaemonStartupErrorCode::InstanceAlreadyRunning, std::move(detail));
            }
        }

        m_settingsRepository =
            m_options.settingsPath.isEmpty()
                ? std::make_unique<SettingsRepository>()
                : std::make_unique<SettingsRepository>(
                      std::make_unique<QSettings>(m_options.settingsPath, QSettings::IniFormat));
        const auto settingsResult = m_settingsRepository->load();
        if (const auto* error = std::get_if<SettingsRepositoryError>(&settingsResult))
            return fail(DaemonStartupErrorCode::SettingsMigration, error->detail);
        m_settingsSnapshot = std::get<SettingsSnapshot>(settingsResult);

        const auto locationResult =
            (m_options.cacheRootPath.isEmpty() ? CacheLocationProvider::forApplication()
                                               : CacheLocationProvider{m_options.cacheRootPath})
                .loadOrCreate();
        if (const auto* error = std::get_if<CacheLocationError>(&locationResult))
            return fail(DaemonStartupErrorCode::CacheLocation, error->detail);

        try
        {
            m_services = std::make_unique<DaemonServices>(std::get<CacheLocation>(locationResult));
            m_remoteActions = std::make_unique<DaemonRemoteActionDispatcher>(
                *m_services, *this, [this] { return currentEpoch(); },
                [this] { return reloadSettings(); }, this);
            m_services->setAccessTokenProvider(
                [this](const std::string_view accountId) -> std::optional<std::string>
                {
                    const auto* connection = connectionForAccount(accountId);
                    if (connection == nullptr || connection->apiKey.isEmpty())
                        return std::nullopt;
                    return connection->apiKey.toStdString();
                });
            m_services->setAuthenticationRefreshHandler(
                [this](std::string accountId, std::string rejectedAccessToken)
                {
                    return refreshOAuthCredentialsFor(std::move(accountId),
                                                      std::move(rejectedAccessToken));
                });
            applySettings();
            m_background = std::make_unique<DaemonBackgroundController>(*m_services, this);
            m_services->commandDispatcher().setEventSink(this);
            connectOperationalEvents();
            m_background->start();
        }
        catch (const std::exception& exception)
        {
            return fail(DaemonStartupErrorCode::CacheOpen, startupDetail(exception));
        }

        m_endpoint =
            std::make_unique<protocol::SocketDaemonEndpoint>(*this, m_options.socket, this);
        connect(m_endpoint.get(), &protocol::SocketDaemonEndpoint::connectionOpened, this,
                [this]
                {
                    m_guiConnected = true;
                    m_guiReady = false;
                    Q_EMIT guiConnected();
                });
        connect(m_endpoint.get(), &protocol::SocketDaemonEndpoint::connectionClosed, this,
                &DaemonProcess::onSocketConnectionClosed);

        if (const auto error = m_endpoint->listen())
            return fail(DaemonStartupErrorCode::SocketListen, error->detail);

        auto activationOptions = m_options.socket;
        activationOptions.socketPath += QStringLiteral(".activation");
        m_activationEndpoint = std::make_unique<protocol::SocketActivationEndpoint>(
            *this, std::move(activationOptions), this);
        if (const auto error = m_activationEndpoint->listen())
            return fail(DaemonStartupErrorCode::SocketListen, error->detail);

        m_lifecycle = DaemonLifecycle::Ready;
        m_oauthRefreshTimer.start();
        QTimer::singleShot(0, this, &DaemonProcess::refreshOAuthCredentials);
        if (PerformanceMetrics::enabled())
        {
            samplePerformance();
            m_performanceTimer.start();
        }
        publishStatus();
        Q_EMIT ready();
        return std::nullopt;
    }

    void DaemonProcess::stop()
    {
        if (m_lifecycle == DaemonLifecycle::ShuttingDown)
            return;
        m_performanceTimer.stop();
        m_oauthRefreshTimer.stop();
        m_oauthRefreshes.cancel();
        if (m_services != nullptr)
            samplePerformance();
        m_lifecycle = DaemonLifecycle::ShuttingDown;
        if (m_background != nullptr)
        {
            m_background->stop();
            m_background.reset();
        }
        if (m_cacheAccessAcknowledged && m_services != nullptr)
        {
            if (const auto error = m_services->cacheAccessBarrier().resume())
                qWarning().noquote()
                    << QStringLiteral("Cache barrier resume during daemon stop:") << error->message;
        }
        m_cacheSuspend.reset();
        m_cacheAccessAcknowledged = false;
        if (m_endpoint != nullptr)
        {
            publishStatus();
            m_endpoint->close();
        }
        if (m_activationEndpoint != nullptr)
            m_activationEndpoint->close();
        if (m_services != nullptr)
            m_services->commandDispatcher().setEventSink(nullptr);
        m_endpoint.reset();
        m_remoteActions.reset();
        m_services.reset();
        m_settingsRepository.reset();
        m_guiConnected = false;
        m_guiReady = false;
        m_guiLaunchRequested = false;
        m_pendingActivations.clear();
        if (m_instanceLock != nullptr)
        {
            m_instanceLock->unlock();
            m_instanceLock.reset();
        }
    }

    bool DaemonProcess::isReady() const
    {
        return m_lifecycle == DaemonLifecycle::Ready;
    }

    bool DaemonProcess::hasGuiConnection() const
    {
        return m_guiConnected;
    }

    void DaemonProcess::enqueueActivation(protocol::ActivationRoute route)
    {
        if (m_guiConnected && m_guiReady)
        {
            onBoundaryEvent(protocol::ActivationRequested{.route = std::move(route)});
            return;
        }

        constexpr std::size_t maximumPendingActivations = 64;
        if (m_pendingActivations.size() >= maximumPendingActivations)
        {
            qWarning() << QStringLiteral("Activation queue is full; dropping the oldest route");
            m_pendingActivations.pop_front();
        }
        m_pendingActivations.push_back(std::move(route));
        if (!m_guiConnected)
            launchGuiIfNeeded();
    }

    std::size_t DaemonProcess::pendingActivationCount() const
    {
        return m_pendingActivations.size();
    }

    const QString& DaemonProcess::databasePath() const
    {
        static const QString empty;
        return m_services == nullptr ? empty : m_services->databasePath();
    }

    const protocol::SettingsSnapshot& DaemonProcess::settings() const
    {
        return m_settingsSnapshot;
    }

    const protocol::DaemonInstanceId& DaemonProcess::instanceId() const
    {
        return m_instanceId;
    }

    protocol::CacheIdentity DaemonProcess::cacheIdentity() const
    {
        if (m_services == nullptr)
            throw std::logic_error("daemon cache is not open");
        return m_services->cacheIdentity();
    }

    protocol::InvalidationEpoch DaemonProcess::currentEpoch() const
    {
        return m_epoch;
    }

    std::optional<protocol::BoundaryError> DaemonProcess::requestCacheAccessSuspend(
        const protocol::CacheSuspendReason reason,
        const std::optional<protocol::CacheSchemaVersion> targetSchema)
    {
        if (!isReady())
            return notReadyError();
        if (!m_guiConnected || !m_guiReady)
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("cache"),
                                 .detail =
                                     QStringLiteral("GUI is not ready to release cache access")};
        if (m_cacheSuspend.has_value())
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("cache"),
                                 .detail =
                                     QStringLiteral("cache access suspension is already pending")};

        m_cacheSuspend = CacheAccessSuspendRequested{
            .instance = cacheIdentity().instance, .reason = reason, .targetSchema = targetSchema};
        m_cacheAccessAcknowledged = false;
        onBoundaryEvent(*m_cacheSuspend);
        return std::nullopt;
    }

    std::optional<protocol::BoundaryError> DaemonProcess::completeCacheAccessResume()
    {
        if (!m_cacheSuspend.has_value() || !m_cacheAccessAcknowledged || m_services == nullptr)
            return BoundaryError{.code = BoundaryErrorCode::InvalidRequest,
                                 .field = QStringLiteral("cache"),
                                 .detail = QStringLiteral(
                                     "cache access cannot resume before the GUI acknowledgement")};
        if (const auto error = m_services->cacheAccessBarrier().resume())
        {
            return BoundaryError{.code = BoundaryErrorCode::CacheUnavailable,
                                 .field = QStringLiteral("cache"),
                                 .detail = error->message};
        }
        m_cacheSuspend.reset();
        m_cacheAccessAcknowledged = false;
        onBoundaryEvent(CacheAccessResumed{.cache = cacheIdentity(),
                                           .cacheDatabasePath = databasePath(),
                                           .epoch = currentEpoch()});
        return std::nullopt;
    }

    protocol::HandshakeReply DaemonProcess::handleHello(const protocol::HelloRequest& request)
    {
        PerformanceSpan metrics{QStringLiteral("daemon"), QStringLiteral("daemon_handshake")};
        if (!isReady())
        {
            metrics.finish(QStringLiteral("unavailable"));
            return HandshakeRejected{.error = notReadyError()};
        }
        if (request.protocol.major != m_options.protocol.major ||
            request.protocol.minor > m_options.protocol.minor)
        {
            metrics.finish(QStringLiteral("rejected"), QStringLiteral("reason=protocol"));
            return HandshakeRejected{
                .error = {.code = BoundaryErrorCode::InvalidProtocol,
                          .field = QStringLiteral("hello.protocol"),
                          .detail = QStringLiteral("daemon protocol is incompatible")}};
        }
        if (request.build != m_options.build)
        {
            metrics.finish(QStringLiteral("rejected"), QStringLiteral("reason=build"));
            return HandshakeRejected{
                .error = {.code = BoundaryErrorCode::IncompatibleBuild,
                          .field = QStringLiteral("hello.build"),
                          .detail = QStringLiteral("daemon build identity is incompatible")}};
        }
        metrics.finish(QStringLiteral("ready"));
        return ReadyReply{.protocol = m_options.protocol,
                          .daemon = m_instanceId,
                          .cache = cacheIdentity(),
                          .cacheDatabasePath = databasePath(),
                          .epoch = currentEpoch(),
                          .settingsRevision = m_settingsSnapshot.revision};
    }

    protocol::CommandReply DaemonProcess::handleCommand(protocol::CommandRequest request)
    {
        if (!isReady())
            return protocol::CommandRejected{.id = request.id, .error = notReadyError()};
        if (std::holds_alternative<protocol::RemoteActionCommand>(request.command))
            return m_remoteActions->dispatch(std::move(request));

        PerformanceSpan metrics{QStringLiteral("daemon"), QStringLiteral("command_admission"),
                                QStringLiteral("kind=refresh_account")};
        auto reply = m_services->commandDispatcher().dispatch(std::move(request));
        if (auto* accepted = std::get_if<protocol::CommandAccepted>(&reply))
            accepted->epoch = currentEpoch();
        metrics.finish(std::holds_alternative<protocol::CommandAccepted>(reply)
                           ? QStringLiteral("accepted")
                           : QStringLiteral("rejected"));
        return reply;
    }

    protocol::MaterializationReply
    DaemonProcess::handleMaterialization(protocol::MaterializationRequest request)
    {
        PerformanceSpan metrics{QStringLiteral("daemon"),
                                QStringLiteral("materialization_admission"),
                                QStringLiteral("kind=mailbox_window")};
        if (!isReady())
        {
            metrics.finish(QStringLiteral("unavailable"));
            return protocol::MaterializationRejected{.id = request.id, .error = notReadyError()};
        }

        const auto* mailbox = std::get_if<protocol::MailboxWindowMaterialization>(&request.request);
        if (mailbox == nullptr)
        {
            metrics.finish(QStringLiteral("rejected"), QStringLiteral("reason=unsupported"));
            return protocol::MaterializationRejected{
                .id = request.id,
                .error = {.code = BoundaryErrorCode::UnsupportedOperation,
                          .field = QStringLiteral("materialization"),
                          .detail = QStringLiteral("materialization kind is not supported")}};
        }

        const auto requestId = request.id;
        const auto startedAt = std::chrono::steady_clock::now();
        auto task = m_services->mailService().requestMailboxWindow({
            .accountId = mailbox->accountId.toStdString(),
            .mailboxId = mailbox->mailboxId.toStdString(),
            .offset = static_cast<std::size_t>(mailbox->offset),
            .limit = static_cast<std::size_t>(mailbox->limit),
            .sort = {},
            .forceRefresh = false,
            .anchor = std::nullopt,
            .anchorOffset = 1,
        });
        QCoro::connect(
            std::move(task), this,
            [this, requestId, startedAt](MailboxWindowResult result)
            {
                QString details;
                if (const auto* summary = std::get_if<MailboxWindowSummary>(&result))
                {
                    details = QStringLiteral("returned=%1 representatives=%2")
                                  .arg(summary->returnedLimit)
                                  .arg(summary->representativeCount);
                }
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    PerformanceMetrics::recordDuration(
                        QStringLiteral("daemon"), QStringLiteral("materialization_completion"),
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - startedAt),
                        QStringLiteral("failed"));
                    onBoundaryEvent(protocol::OperationFailed{
                        .operation = protocol::OperationId{.value = requestId.value},
                        .error = {.code = protocol::BoundaryErrorCode::TransportUnavailable,
                                  .field = QStringLiteral("materialization"),
                                  .detail = error->message}});
                }
                else
                {
                    PerformanceMetrics::recordDuration(
                        QStringLiteral("daemon"), QStringLiteral("materialization_completion"),
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - startedAt),
                        QStringLiteral("completed"), std::move(details));
                }
            });
        metrics.finish(QStringLiteral("accepted"));
        return protocol::MaterializationAccepted{.id = request.id};
    }

    void DaemonProcess::handleCancelMaterializationScope(
        const protocol::CancelMaterializationScopeRequest&)
    {
    }

    protocol::SettingsReadReply
    DaemonProcess::handleGetSettings(const protocol::GetSettingsRequest&)
    {
        if (!isReady())
            return protocol::SettingsReadRejected{.error = notReadyError()};
        return protocol::SettingsSnapshotReply{.snapshot = m_settingsSnapshot};
    }

    protocol::SettingsUpdateReply
    DaemonProcess::handleUpdateSettings(protocol::UpdateSettingsRequest request)
    {
        if (!isReady())
            return protocol::SettingsUpdateRejected{.currentRevision = m_settingsSnapshot.revision,
                                                    .error = notReadyError()};
        const auto previousAccounts = m_settingsSnapshot.accounts;
        const auto previousSyncedMailboxes = m_settingsSnapshot.syncedMailboxSelections;
        const auto previousNotificationMailboxes = m_settingsSnapshot.notificationMailboxSelections;
        const auto reply = m_settingsRepository->update(std::move(request));
        if (const auto* updated = std::get_if<protocol::SettingsUpdated>(&reply))
        {
            const auto loaded = m_settingsRepository->load();
            if (const auto* error = std::get_if<SettingsRepositoryError>(&loaded))
            {
                return protocol::SettingsUpdateRejected{
                    .currentRevision = m_settingsSnapshot.revision, .error = settingsError(*error)};
            }
            m_settingsSnapshot = std::get<protocol::SettingsSnapshot>(loaded);
            if (m_settingsSnapshot.accounts != previousAccounts ||
                m_settingsSnapshot.syncedMailboxSelections != previousSyncedMailboxes ||
                m_settingsSnapshot.notificationMailboxSelections != previousNotificationMailboxes)
                applySettings();
            onBoundaryEvent(*updated);
        }
        return reply;
    }

    std::optional<protocol::BoundaryError> DaemonProcess::handleCacheAccessSuspended(
        const protocol::CacheAccessSuspendedAcknowledgement& acknowledgement)
    {
        if (!m_cacheSuspend.has_value())
            return BoundaryError{.code = BoundaryErrorCode::InvalidRequest,
                                 .field = QStringLiteral("cache.instance"),
                                 .detail = QStringLiteral("no cache suspension is pending")};
        if (acknowledgement.instance != m_cacheSuspend->instance)
            return BoundaryError{.code = BoundaryErrorCode::InvalidIdentifier,
                                 .field = QStringLiteral("cache.instance"),
                                 .detail =
                                     QStringLiteral("cache instance acknowledgement is stale")};

        if (const auto error = m_services->cacheAccessBarrier().suspend())
        {
            m_cacheSuspend.reset();
            return BoundaryError{.code = BoundaryErrorCode::CacheUnavailable,
                                 .field = QStringLiteral("cache"),
                                 .detail = error->message};
        }
        m_cacheAccessAcknowledged = true;
        return std::nullopt;
    }

    std::optional<protocol::BoundaryError> DaemonProcess::handlePing(const protocol::PingRequest&)
    {
        return isReady() ? std::optional<protocol::BoundaryError>{}
                         : std::optional<protocol::BoundaryError>{notReadyError()};
    }

    std::optional<protocol::BoundaryError> DaemonProcess::handleGuiReadyForActivation()
    {
        if (!isReady())
            return notReadyError();
        if (m_guiReady)
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("gui"),
                                 .detail =
                                     QStringLiteral("GUI activation was already acknowledged")};
        m_guiReady = true;
        flushPendingActivations();
        publishStatus();
        return std::nullopt;
    }

    std::optional<protocol::BoundaryError>
    DaemonProcess::handleGuiActivation(const protocol::ActivationRoute& route)
    {
        if (!isReady())
            return BoundaryError{.code = BoundaryErrorCode::Busy,
                                 .field = QStringLiteral("gui"),
                                 .detail = QStringLiteral("daemon is not ready to activate GUI")};
        if (m_guiConnected && m_guiReady)
        {
            onBoundaryEvent(ActivationRequested{.route = route});
            return std::nullopt;
        }
        if (m_guiConnected)
        {
            enqueueActivation(route);
            return std::nullopt;
        }
        return BoundaryError{
            .code = BoundaryErrorCode::Busy,
            .field = QStringLiteral("gui"),
            .detail = QStringLiteral("no GUI process is currently connected"),
        };
    }

    void DaemonProcess::onBoundaryEvent(const protocol::BoundaryEvent& event)
    {
        if (m_endpoint != nullptr)
            m_endpoint->publishEvent(event);
    }

    std::optional<DaemonStartupError> DaemonProcess::fail(const DaemonStartupErrorCode code,
                                                          QString detail)
    {
        m_lifecycle = DaemonLifecycle::Recovering;
        return DaemonStartupError{.code = code, .detail = std::move(detail)};
    }

    protocol::BoundaryError DaemonProcess::notReadyError() const
    {
        const auto detail = m_lifecycle == protocol::DaemonLifecycle::ShuttingDown
                                ? QStringLiteral("daemon is shutting down")
                                : QStringLiteral("daemon is not ready");
        return {.code = m_lifecycle == protocol::DaemonLifecycle::ShuttingDown
                            ? BoundaryErrorCode::DaemonShuttingDown
                            : BoundaryErrorCode::CacheUnavailable,
                .field = QStringLiteral("daemon"),
                .detail = detail};
    }

    protocol::DaemonStatus DaemonProcess::daemonStatus() const
    {
        protocol::DaemonStatus status{.lifecycle = m_lifecycle, .accounts = {}};
        if (m_services == nullptr)
            return status;
        for (const auto& [accountId, accountStatus] :
             m_services->mailApplicationEvents().accountStatuses())
        {
            status.accounts.push_back({.accountId = QString::fromStdString(accountId),
                                       .state = accountState(accountStatus),
                                       .detail = {}});
        }
        std::ranges::sort(status.accounts, [](const auto& left, const auto& right)
                          { return left.accountId < right.accountId; });
        return status;
    }

    void DaemonProcess::publishStatus()
    {
        onBoundaryEvent(DaemonStatusChanged{.status = daemonStatus()});
    }

    void DaemonProcess::applySettings()
    {
        const auto completeSettings = std::ranges::count_if(
            m_settingsSnapshot.accounts, [](const auto& account)
            { return !account.loginEmail.isEmpty() && !account.apiKey.isEmpty(); });
        qInfo().noquote() << QStringLiteral(
                                 "Loaded %1 configured connection%2 (%3 with credentials)")
                                 .arg(m_settingsSnapshot.accounts.size())
                                 .arg(m_settingsSnapshot.accounts.size() == 1 ? QString{}
                                                                              : QStringLiteral("s"))
                                 .arg(completeSettings);
        const auto configurations = accountConfigurations(m_settingsSnapshot);
        std::vector<FullSyncAccountConfiguration> fullSync;
        std::vector<std::string> accountIds;
        fullSync.reserve(configurations.size());
        accountIds.reserve(configurations.size());
        for (const auto& configuration : configurations)
        {
            fullSync.push_back({.settings = configuration.settings,
                                .accountId = configuration.accountId,
                                .mailboxIds = configuration.fullSyncMailboxIds});
            accountIds.push_back(configuration.accountId);
        }
        QStringList configuredAccountIds;
        configuredAccountIds.reserve(static_cast<qsizetype>(configurations.size()));
        for (const auto& configuration : configurations)
            configuredAccountIds.push_back(QString::fromStdString(configuration.accountId));
        qInfo().noquote() << QStringLiteral("Configured %1 JMAP account%2: %3")
                                 .arg(configurations.size())
                                 .arg(configurations.size() == 1 ? QString{} : QStringLiteral("s"),
                                      configuredAccountIds.join(QStringLiteral(", ")));
        m_services->mailService().applySettings(configurations);
        m_services->fullMailSyncService().applySettings(std::move(fullSync));
        m_services->mailIndexService().applyAccounts(std::move(accountIds));
        m_services->localMaintenanceService().requestReplay();
    }

    void DaemonProcess::refreshOAuthCredentials()
    {
        if (!isReady() || m_services == nullptr || m_settingsRepository == nullptr)
            return;
        const auto refreshBefore = QDateTime::currentSecsSinceEpoch() + 300;
        for (const auto& account : m_settingsSnapshot.accounts)
        {
            if (account.reauthenticationRequired || account.refreshToken.isEmpty() ||
                account.tokenEndpoint.isEmpty() || account.oauthClientId.isEmpty() ||
                account.tokenExpiresAtEpochSeconds == 0 ||
                account.tokenExpiresAtEpochSeconds > refreshBefore)
            {
                continue;
            }

            auto task = startOAuthRefresh(account.id, false);
            QCoro::connect(std::move(task), this, [](OAuthRefreshOutcome) {});
        }
    }

    QCoro::Task<OAuthRefreshOutcome> DaemonProcess::startOAuthRefresh(QString connectionId,
                                                                      const bool force)
    {
        if (!isReady() || m_services == nullptr || m_settingsRepository == nullptr)
            co_return OAuthRefreshOutcome{};

        const auto account = std::ranges::find(m_settingsSnapshot.accounts, connectionId,
                                               &protocol::AccountSettings::id);
        if (account == m_settingsSnapshot.accounts.end() || account->reauthenticationRequired ||
            account->refreshToken.isEmpty() || account->tokenEndpoint.isEmpty() ||
            account->oauthClientId.isEmpty())
        {
            co_return OAuthRefreshOutcome{};
        }
        if (!force &&
            (account->tokenExpiresAtEpochSeconds == 0 ||
             account->tokenExpiresAtEpochSeconds > QDateTime::currentSecsSinceEpoch() + 300))
        {
            co_return OAuthRefreshOutcome{};
        }

        const auto sessionUrl = account->sessionUrl;
        const auto refreshToken = account->refreshToken;
        const auto tokenEndpoint = account->tokenEndpoint;
        const auto clientId = account->oauthClientId;
        const auto resourceUrl = account->oauthResource;
        const auto scope = account->oauthScope;
        const auto previousAccessToken = account->apiKey;
        const auto refreshKey = connectionId;
        co_return co_await m_oauthRefreshes.run(
            refreshKey,
            [this, connectionId = std::move(connectionId), sessionUrl, refreshToken, tokenEndpoint,
             clientId, resourceUrl, scope, previousAccessToken]() mutable
            {
                return performOAuthRefresh(
                    std::move(connectionId), std::move(sessionUrl), std::move(refreshToken),
                    std::move(tokenEndpoint), std::move(clientId), std::move(resourceUrl),
                    std::move(scope), std::move(previousAccessToken));
            });
    }

    QCoro::Task<OAuthRefreshOutcome>
    DaemonProcess::performOAuthRefresh(QString connectionId, QString sessionUrl,
                                       QString refreshToken, QString tokenEndpoint,
                                       QString clientId, QString resourceUrl, QString scope,
                                       QString previousAccessToken)
    {
        auto result = co_await m_services->onboardingService().refreshOAuth({
            .sessionUrl = std::move(sessionUrl),
            .tokenEndpoint = tokenEndpoint,
            .clientId = clientId,
            .refreshToken = refreshToken,
            .resourceUrl = resourceUrl,
            .scope = scope,
        });

        OAuthRefreshOutcome outcome;
        auto accounts = m_settingsSnapshot.accounts;
        const auto found =
            std::ranges::find(accounts, connectionId, &protocol::AccountSettings::id);
        if (found != accounts.end() && found->apiKey != previousAccessToken)
        {
            co_return OAuthRefreshOutcome{.succeeded = !found->apiKey.isEmpty(),
                                          .accessToken = found->apiKey};
        }
        if (result.succeeded && isReady() && found != accounts.end() &&
            found->refreshToken == refreshToken && found->tokenEndpoint == tokenEndpoint &&
            found->oauthClientId == clientId && found->oauthResource == resourceUrl)
        {
            const auto refreshedAccessToken = result.accessToken;
            found->apiKey = result.accessToken;
            found->refreshToken = result.refreshToken;
            found->oauthScope = result.scope;
            found->tokenExpiresAtEpochSeconds = result.expiresAtEpochSeconds;
            found->reauthenticationRequired = false;
            ++found->revision;
            protocol::SettingsUpdate update;
            update.accounts = std::move(accounts);
            const auto updateResult = handleUpdateSettings(
                {.baseRevision = m_settingsSnapshot.revision, .update = std::move(update)});
            if (std::holds_alternative<protocol::SettingsUpdated>(updateResult))
            {
                co_return OAuthRefreshOutcome{.succeeded = true,
                                              .accessToken = refreshedAccessToken};
            }

            const auto& rejected = std::get<protocol::SettingsUpdateRejected>(updateResult);
            qWarning().noquote() << QStringLiteral("OAuth refresh credentials were not applied")
                                 << connectionId << rejected.error.detail;
        }
        else if (!result.succeeded)
        {
            qWarning().noquote() << QStringLiteral("OAuth credential refresh failed")
                                 << connectionId << result.error;
            if (result.failureKind == OAuthRefreshFailureKind::ReauthenticationRequired &&
                isReady() && found != accounts.end())
            {
                found->reauthenticationRequired = true;
                ++found->revision;
                protocol::SettingsUpdate update;
                update.accounts = std::move(accounts);
                const auto updateResult = handleUpdateSettings(
                    {.baseRevision = m_settingsSnapshot.revision, .update = std::move(update)});
                if (!std::holds_alternative<protocol::SettingsUpdated>(updateResult))
                {
                    const auto& rejected =
                        std::get<protocol::SettingsUpdateRejected>(updateResult);
                    qWarning().noquote()
                        << QStringLiteral("OAuth reauthentication state was not saved")
                        << connectionId << rejected.error.detail;
                }
            }
        }
        else if (found != accounts.end())
        {
            qWarning().noquote() << QStringLiteral("Discarded stale OAuth refresh result")
                                 << connectionId;
        }

        co_return outcome;
    }

    const protocol::AccountSettings*
    DaemonProcess::connectionForAccount(const std::string_view accountId) const
    {
        const auto key = QString::fromStdString(std::string{accountId});
        for (const auto& connection : m_settingsSnapshot.accounts)
        {
            if (connection.id == key || std::ranges::find(connection.cachedAccountIds, key) !=
                                            connection.cachedAccountIds.end())
            {
                return &connection;
            }
        }
        return nullptr;
    }

    QCoro::Task<std::optional<std::string>>
    DaemonProcess::refreshOAuthCredentialsFor(std::string accountId,
                                              std::string rejectedAccessToken)
    {
        if (!isReady() || m_services == nullptr || m_settingsRepository == nullptr)
            co_return std::nullopt;

        const auto* connection = connectionForAccount(accountId);
        if (connection == nullptr || connection->apiKey.isEmpty())
            co_return std::nullopt;
        if (connection->apiKey.toStdString() != rejectedAccessToken)
            co_return connection->apiKey.toStdString();

        auto outcome = co_await startOAuthRefresh(connection->id, true);
        if (!outcome.succeeded || outcome.accessToken.isEmpty())
            co_return std::nullopt;
        co_return outcome.accessToken.toStdString();
    }

    std::optional<protocol::BoundaryError> DaemonProcess::reloadSettings()
    {
        if (!isReady() || m_settingsRepository == nullptr || m_services == nullptr)
            return notReadyError();
        const auto loaded = m_settingsRepository->load();
        if (const auto* error = std::get_if<SettingsRepositoryError>(&loaded))
            return settingsError(*error);
        m_settingsSnapshot = std::get<protocol::SettingsSnapshot>(loaded);
        applySettings();
        onBoundaryEvent(protocol::SettingsUpdated{.revision = m_settingsSnapshot.revision});
        return std::nullopt;
    }

    void DaemonProcess::connectOperationalEvents()
    {
        auto& events = m_services->mailApplicationEvents();
        connect(&events, &MailApplicationEventsPort::cacheInvalidated, this,
                [this](MailCacheInvalidation invalidation)
                {
                    std::vector<QString> mailboxIds{invalidation.change.mailboxIds.begin(),
                                                    invalidation.change.mailboxIds.end()};
                    std::vector<MailboxWindowInvalidation> mailboxWindows;
                    mailboxWindows.reserve(invalidation.change.queryWindows.size());
                    for (const auto& window : invalidation.change.queryWindows)
                    {
                        mailboxWindows.push_back({
                            .mailboxId = window.mailboxId,
                            .offset = static_cast<std::uint64_t>(window.offset),
                            .limit = static_cast<std::uint64_t>(window.limit),
                            .total = window.total.transform(
                                [](const std::size_t total)
                                { return static_cast<std::uint64_t>(total); }),
                        });
                    }
                    std::vector<SearchWindowInvalidation> searchWindows;
                    searchWindows.reserve(invalidation.change.searchWindows.size());
                    for (const auto& window : invalidation.change.searchWindows)
                    {
                        searchWindows.push_back({
                            .queryKey = window.queryKey,
                            .offset = static_cast<std::uint64_t>(window.offset),
                            .limit = static_cast<std::uint64_t>(window.limit),
                            .total = window.total.transform(
                                [](const std::size_t total)
                                { return static_cast<std::uint64_t>(total); }),
                        });
                    }
                    ++m_epoch.value;
                    onBoundaryEvent(CacheInvalidation{
                        .epoch = currentEpoch(),
                        .changedDomains = std::move(invalidation.changedDomains),
                        .affectedKeys = std::move(invalidation.affectedKeys),
                        .accountId = std::move(invalidation.change.accountId),
                        .mailboxIds = std::move(mailboxIds),
                        .mailboxWindows = std::move(mailboxWindows),
                        .searchWindows = std::move(searchWindows),
                    });
                });
        connect(&events, &MailApplicationEventsPort::accountStatusChanged, this,
                [this](const QString&, MailAccountStatus) { publishStatus(); });
        connect(&m_services->undoCommandPort(), &UndoCommandPort::historyStateChanged, this,
                [this](const undo::HistoryState&)
                {
                    ++m_epoch.value;
                    onBoundaryEvent(CacheInvalidation{
                        .epoch = currentEpoch(),
                        .changedDomains = {ChangedDomain::History},
                        .affectedKeys = {},
                    });
                });
        connect(&m_services->workScheduler(), &WorkScheduler::jobsChanged, this,
                [this]
                {
                    ++m_epoch.value;
                    onBoundaryEvent(CacheInvalidation{
                        .epoch = currentEpoch(),
                        .changedDomains = {ChangedDomain::BackgroundJobs},
                        .affectedKeys = {},
                    });
                });
        connect(m_background.get(), &DaemonBackgroundController::activationRequested, this,
                &DaemonProcess::enqueueActivation);
        connect(m_background.get(), &DaemonBackgroundController::shutdownRequested, this,
                &DaemonProcess::requestShutdown);
    }

    void DaemonProcess::onSocketConnectionClosed(const protocol::SocketDisconnectReason,
                                                 const QString&)
    {
        const bool wasConnected = m_guiConnected;
        if (m_cacheAccessAcknowledged && m_services != nullptr)
        {
            if (const auto error = m_services->cacheAccessBarrier().resume())
                qWarning().noquote() << QStringLiteral("Cache barrier resume after GUI disconnect:")
                                     << error->message;
        }
        if (m_remoteActions != nullptr)
            m_remoteActions->releaseGuiResources();
        m_guiConnected = false;
        m_guiReady = false;
        m_guiLaunchRequested = false;
        m_cacheSuspend.reset();
        m_cacheAccessAcknowledged = false;
        if (!m_pendingActivations.empty())
            QTimer::singleShot(0, this, &DaemonProcess::launchGuiIfNeeded);
        if (wasConnected)
        {
            Q_EMIT guiDisconnected();
            publishStatus();
        }
    }

    void DaemonProcess::flushPendingActivations()
    {
        if (!m_guiConnected || !m_guiReady)
            return;
        while (!m_pendingActivations.empty())
        {
            onBoundaryEvent(
                protocol::ActivationRequested{.route = std::move(m_pendingActivations.front())});
            m_pendingActivations.pop_front();
        }
    }

    void DaemonProcess::launchGuiIfNeeded()
    {
        if (m_guiConnected || m_guiLaunchRequested || m_pendingActivations.empty())
            return;
        if (m_options.guiExecutable.isEmpty())
        {
            qWarning() << QStringLiteral(
                "Activation is queued but no GUI executable was configured");
            return;
        }
        const QStringList arguments{QStringLiteral("--runtime-directory"),
                                    m_options.socket.runtimeDirectory, QStringLiteral("--socket"),
                                    m_options.socket.socketPath};
        if (!QProcess::startDetached(m_options.guiExecutable, arguments))
        {
            qWarning().noquote() << QStringLiteral("Could not start GUI for activation:")
                                 << m_options.guiExecutable;
            return;
        }
        m_guiLaunchRequested = true;
    }

    void DaemonProcess::requestShutdown()
    {
        if (!isReady())
            return;
        onBoundaryEvent(protocol::DaemonShutdownRequested{});
        Q_EMIT shutdownRequested();
    }

    void DaemonProcess::samplePerformance()
    {
        if (!PerformanceMetrics::enabled() || m_services == nullptr)
            return;

        PerformanceMetrics::recordProcessResources(QStringLiteral("daemon"), databasePath());
        const auto& scheduler = m_services->workScheduler();
        const auto metrics = scheduler.admissionMetrics();
        const auto details =
            QStringLiteral("active=%1 admitted=%2 completed=%3 rejected=%4 "
                           "queue_wait_total_us=%5 queue_wait_max_us=%6 "
                           "transaction_total_us=%7 foreground_total_us=%8 "
                           "foreground_admission_total_us=%9")
                .arg(static_cast<qulonglong>(scheduler.activeAdmissions()))
                .arg(static_cast<qulonglong>(metrics.admitted))
                .arg(static_cast<qulonglong>(metrics.completed))
                .arg(static_cast<qulonglong>(metrics.rejected))
                .arg(static_cast<qlonglong>(metrics.totalQueueWait.count()))
                .arg(static_cast<qlonglong>(metrics.maximumQueueWait.count()))
                .arg(static_cast<qlonglong>(metrics.totalTransactionTime.count()))
                .arg(static_cast<qlonglong>(metrics.totalForegroundTime.count()))
                .arg(static_cast<qlonglong>(metrics.totalForegroundAdmissionLatency.count()));
        PerformanceMetrics::recordEvent(QStringLiteral("daemon"),
                                        QStringLiteral("work_scheduler_snapshot"),
                                        QStringLiteral("sample"), details);
    }
} // namespace javelin::app
