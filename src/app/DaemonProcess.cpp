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
#include "app/SettingsRepository.h"
#include "app/TranslationApplicationPorts.h"
#include "app/TranslationService.h"
#include "app/UndoApplicationPorts.h"
#include "app/WorkScheduler.h"
#include "app/undo/UndoManager.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QDir>
#include <QLockFile>
#include <QMetaObject>
#include <QProcess>
#include <QSettings>
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
                    .apiKey = settings.apiKey.toStdString()};
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

        [[nodiscard]] QStringList stringList(const std::vector<QString>& values)
        {
            return {values.begin(), values.end()};
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
                        .notificationMailboxSelectionConfigured =
                            notifications != nullptr && notifications->configured,
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
    } // namespace

    DaemonProcess::DaemonProcess(DaemonProcessOptions options, QObject* parent)
        : QObject(parent), m_options(std::move(options))
    {
        if (!m_options.socket.expectedBuild.has_value())
            m_options.socket.expectedBuild = m_options.build;
        m_options.socket.protocol = m_options.protocol;
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

            qint64 pid = 0;
            QString hostname;
            QString applicationName;
            auto detail = QStringLiteral("another javelind instance is already running");
            if (m_instanceLock->getLockInfo(&pid, &hostname, &applicationName))
                detail += QStringLiteral(" (pid %1)").arg(pid);
            return fail(DaemonStartupErrorCode::InstanceAlreadyRunning, std::move(detail));
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
        publishStatus();
        Q_EMIT ready();
        return std::nullopt;
    }

    void DaemonProcess::stop()
    {
        if (m_lifecycle == DaemonLifecycle::ShuttingDown)
            return;
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
        if (!isReady())
            return HandshakeRejected{.error = notReadyError()};
        if (request.protocol.major != m_options.protocol.major ||
            request.protocol.minor > m_options.protocol.minor)
        {
            return HandshakeRejected{
                .error = {.code = BoundaryErrorCode::InvalidProtocol,
                          .field = QStringLiteral("hello.protocol"),
                          .detail = QStringLiteral("daemon protocol is incompatible")}};
        }
        if (request.build != m_options.build)
        {
            return HandshakeRejected{
                .error = {.code = BoundaryErrorCode::IncompatibleBuild,
                          .field = QStringLiteral("hello.build"),
                          .detail = QStringLiteral("daemon build identity is incompatible")}};
        }
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

        auto reply = m_services->commandDispatcher().dispatch(std::move(request));
        if (auto* accepted = std::get_if<protocol::CommandAccepted>(&reply))
            accepted->epoch = currentEpoch();
        return reply;
    }

    protocol::MaterializationReply
    DaemonProcess::handleMaterialization(protocol::MaterializationRequest request)
    {
        if (!isReady())
            return protocol::MaterializationRejected{.id = request.id, .error = notReadyError()};

        const auto* mailbox = std::get_if<protocol::MailboxWindowMaterialization>(&request.request);
        if (mailbox == nullptr)
            return protocol::MaterializationRejected{
                .id = request.id,
                .error = {.code = BoundaryErrorCode::UnsupportedOperation,
                          .field = QStringLiteral("materialization"),
                          .detail = QStringLiteral("materialization kind is not supported")}};

        const auto requestId = request.id;
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
            [this, requestId](MailboxWindowResult result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    onBoundaryEvent(protocol::OperationFailed{
                        .operation = protocol::OperationId{.value = requestId.value},
                        .error = {.code = protocol::BoundaryErrorCode::TransportUnavailable,
                                  .field = QStringLiteral("materialization"),
                                  .detail = error->message}});
                }
            });
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
        const auto& translation = m_settingsSnapshot.translation;
        m_services->translationService().applySettings({
            .enabled = translation.enabled,
            .apiKeyOverride = translation.apiKeyOverride,
            .targetLanguage = translation.targetLanguage,
            .autoTranslateSenders = stringList(translation.autoTranslateSenders),
            .autoTranslateDomains = stringList(translation.autoTranslateDomains),
        });
        m_services->mailService().applySettings(configurations);
        m_services->fullMailSyncService().applySettings(std::move(fullSync));
        m_services->mailIndexService().applyAccounts(std::move(accountIds));
        m_services->localMaintenanceService().requestReplay();
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
                    ++m_epoch.value;
                    onBoundaryEvent(CacheInvalidation{
                        .epoch = currentEpoch(),
                        .changedDomains = std::move(invalidation.changedDomains),
                        .affectedKeys = std::move(invalidation.affectedKeys),
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
} // namespace javelin::app
