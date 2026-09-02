#include "daemon/DaemonServices.h"

#include "app/AccountCommandService.h"
#include "app/AccountRefreshCommandService.h"
#include "app/AccountRuntimeManager.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/CacheAccessBarrier.h"
#include "app/CacheLocationProvider.h"
#include "app/CalendarApplicationService.h"
#include "app/CalendarCommandService.h"
#include "app/CalendarInvitationService.h"
#include "app/CalendarNotificationService.h"
#include "app/CommandDispatcher.h"
#include "app/ComposeCommandService.h"
#include "app/ComposeService.h"
#include "app/ContactApplicationService.h"
#include "app/ContactCommandService.h"
#include "app/DeferredSendRepository.h"
#include "app/DeferredSendService.h"
#include "app/DeveloperDiagnosticsService.h"
#include "app/DeveloperMaintenanceService.h"
#include "app/FullMailSyncService.h"
#include "app/IdentityCommandService.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationEventsService.h"
#include "app/MailCommandService.h"
#include "app/MailExportService.h"
#include "app/MailImportService.h"
#include "app/MailIndexService.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailNotificationService.h"
#include "app/MailQueryApplicationService.h"
#include "app/MailTransferCommandService.h"
#include "app/MailTransferWorkService.h"
#include "app/MailboxMaintenanceRegistry.h"
#include "app/MessageContentApplicationService.h"
#include "app/MessageContentCommandService.h"
#include "app/MessageNavigationCoordinator.h"
#include "app/SieveApplicationService.h"
#include "app/SieveCommandService.h"
#include "app/ThreadMaterializationCoordinator.h"
#include "app/ThreadMembershipMaterializationWorker.h"
#include "app/UndoCommandService.h"
#include "app/WorkScheduler.h"
#include "app/undo/AddressBookHistoryExecutor.h"
#include "app/undo/CalendarHistoryExecutor.h"
#include "app/undo/CalendarPreferenceExecutor.h"
#include "app/undo/ContactHistoryExecutor.h"
#include "app/undo/DraftHistoryExecutor.h"
#include "app/undo/HistoryRepository.h"
#include "app/undo/MailHistoryExecutor.h"
#include "app/undo/MailTransferHistoryCoordinator.h"
#include "app/undo/MailTransferHistoryExecutor.h"
#include "app/undo/MailTransferHistoryService.h"
#include "app/undo/SieveHistoryExecutor.h"
#include "app/undo/UndoManager.h"

#include "jmap/AccountBootstrapClient.h"
#include "jmap/MessageContentClient.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/SessionRefreshClient.h"
#include "jmap/api/Transport.h"
#include "jmap/auth/AccountOnboardingService.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailTagReadRepository.h"
#include "jmap/cache/MailVault.h"
#include "jmap/cache/MailboxFilterReadRepository.h"
#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxStatisticsReadRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/SubmissionRepository.h"
#include "jmap/calendar/CalendarCacheReader.h"
#include "jmap/calendar/CalendarMutationEngine.h"
#include "jmap/calendar/CalendarProtocolClient.h"
#include "jmap/calendar/CalendarSyncEngine.h"
#include "jmap/contacts/ContactMediaService.h"
#include "jmap/contacts/ContactMutationEngine.h"
#include "jmap/contacts/ContactProtocolClient.h"
#include "jmap/contacts/ContactSyncEngine.h"
#include "jmap/identity/IdentityService.h"
#include "jmap/query/MailQueryClient.h"
#include "jmap/query/MailQueryMaterializer.h"
#include "jmap/sieve/SieveMutationEngine.h"
#include "jmap/sieve/SieveProtocolClient.h"
#include "jmap/submission/ComposeService.h"
#include "jmap/sync/EmailMutationEngine.h"
#include "jmap/sync/MailboxMutationEngine.h"
#include "jmap/sync/MutationJournal.h"

#include <QCoroTask>

#include <QDebug>

#include <cstdint>
#include <memory>
#include <stdexcept>

namespace javelin::app
{

    namespace
    {

        [[nodiscard]] CacheLocation cacheLocation()
        {
            const auto result = CacheLocationProvider::forApplication().loadOrCreate();
            if (const auto* error = std::get_if<CacheLocationError>(&result))
                throw std::runtime_error(error->detail.toStdString());
            return std::get<CacheLocation>(result);
        }

    } // namespace

    DaemonServices::DaemonServices() : DaemonServices(cacheLocation())
    {
    }

    DaemonServices::DaemonServices(CacheLocation location)
    {
        auto databaseResult = javelin::jmap::cache::DaemonDatabaseFactory{
            javelin::jmap::cache::DatabaseConnectionOptions{
                .connectionName = QStringLiteral("javelin-daemon-main"),
                .databasePath = location.databasePath,
            }}.open();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&databaseResult))
        {
            throw std::runtime_error(error->message.toStdString());
        }

        m_databaseConnection =
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(databaseResult));
        if (const auto cleanupError =
                javelin::jmap::cache::MailVault::forDatabase(m_databaseConnection)
                    .cleanupIncoming())
            qWarning().noquote() << cleanupError->message;
        m_databasePath = location.databasePath;
        m_cacheInstanceId = location.instanceId;
        m_developerDiagnosticsService = std::make_unique<DeveloperDiagnosticsService>(
            location.databasePath, location.vaultRootPath);
        m_cacheAccessBarrier = std::make_unique<CacheAccessBarrier>();
        m_mailboxMaintenanceRegistry = std::make_unique<MailboxMaintenanceRegistry>();
        m_workScheduler = std::make_unique<WorkScheduler>(m_databaseConnection);
        m_threadMaterializationCoordinator = std::make_unique<ThreadMaterializationCoordinator>(
            m_databaseConnection, *m_workScheduler);
        m_localMaintenanceService =
            std::make_unique<LocalMaintenanceService>(m_databaseConnection, *m_workScheduler);
        javelin::jmap::sync::MutationJournalRepository mutationJournal{m_databaseConnection};
        const auto recoveredMutations = mutationJournal.recoverInFlight();
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&recoveredMutations))
        {
            throw std::runtime_error(error->message.toStdString());
        }
        m_historyRepository =
            std::make_unique<javelin::app::undo::HistoryRepository>(m_databaseConnection);
        m_undoManager = std::make_unique<javelin::app::undo::UndoManager>(*m_historyRepository);
        m_undoCommandService = std::make_unique<UndoCommandService>(*m_undoManager);
        m_deferredSendRepository = std::make_unique<DeferredSendRepository>(m_databaseConnection);
        if (const auto historyError = m_undoManager->load())
            throw std::runtime_error(historyError->message.toStdString());
        m_mailTransferHistoryCoordinator =
            std::make_unique<javelin::app::undo::MailTransferHistoryCoordinator>(
                m_databaseConnection, *m_undoManager);
        m_networkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_stateChangeNetworkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_onboardingService = std::make_unique<javelin::jmap::auth::AccountOnboardingService>(
            *m_networkAccessManager);
        m_webSocketFailureCooldowns =
            std::make_unique<javelin::jmap::api::WebSocketFailureCooldowns>();
        m_networkTransport =
            std::make_unique<javelin::jmap::api::QtNetworkTransport>(*m_networkAccessManager);
        m_transport =
            std::make_unique<javelin::jmap::api::RefreshingTransport>(*m_networkTransport);
        m_httpMethodTransport =
            std::make_unique<javelin::jmap::api::HttpJmapMethodTransport>(*m_transport);
        m_preferredMethodTransport =
            std::make_unique<javelin::jmap::api::PreferredJmapMethodTransport>(
                m_databaseConnection, *m_httpMethodTransport, *m_webSocketFailureCooldowns);
        m_methodTransport = std::make_unique<javelin::jmap::api::RefreshingJmapMethodTransport>(
            *m_preferredMethodTransport);
        m_sessionRefreshClient = std::make_unique<javelin::jmap::SessionRefreshClient>(
            m_databaseConnection, *m_transport);
        m_accountBootstrapClient = std::make_unique<javelin::jmap::AccountBootstrapClient>(
            m_databaseConnection, *m_transport, *m_methodTransport);
        m_mailQueryClient = std::make_unique<javelin::jmap::MailQueryClient>(m_databaseConnection,
                                                                             *m_methodTransport);
        m_mailQueryMaterializer = std::make_unique<javelin::jmap::MailQueryMaterializer>(
            m_databaseConnection, *m_mailQueryClient);
        m_messageContentClient = std::make_unique<javelin::jmap::MessageContentClient>(
            m_databaseConnection, *m_transport);
        m_emailMutationEngine = std::make_unique<javelin::jmap::EmailMutationEngine>(
            m_databaseConnection, *m_methodTransport);
        m_mailboxMutationEngine = std::make_unique<javelin::jmap::MailboxMutationEngine>(
            m_databaseConnection, *m_methodTransport);
        m_mailIndexService =
            std::make_unique<MailIndexService>(m_databaseConnection, *m_workScheduler);
        m_fullMailSyncService = std::make_unique<FullMailSyncService>(
            m_databaseConnection, *m_mailQueryClient, *m_messageContentClient, *m_workScheduler,
            *m_mailIndexService);
        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountRepository>(m_databaseConnection);
        m_accountCommandService = std::make_unique<AccountCommandService>(*m_accountRepository);
        m_contactRepository =
            std::make_unique<javelin::jmap::cache::ContactRepository>(m_databaseConnection);
        m_contactProtocolClient =
            std::make_unique<javelin::jmap::contacts::ContactProtocolClient>(*m_methodTransport);
        m_contactSyncEngine = std::make_unique<javelin::jmap::contacts::ContactSyncEngine>(
            m_databaseConnection, *m_contactRepository, *m_contactProtocolClient);
        m_contactMutationEngine = std::make_unique<javelin::jmap::contacts::ContactMutationEngine>(
            m_databaseConnection, *m_contactRepository, *m_contactProtocolClient,
            *m_contactSyncEngine);
        m_contactMediaService = std::make_unique<javelin::jmap::contacts::ContactMediaService>(
            m_databaseConnection, *m_transport);
        m_calendarReader =
            std::make_unique<javelin::jmap::calendar::CalendarCacheReader>(m_databaseConnection);
        m_calendarProtocolClient =
            std::make_unique<javelin::jmap::calendar::CalendarProtocolClient>(m_databaseConnection,
                                                                              *m_methodTransport);
        m_calendarSyncEngine = std::make_unique<javelin::jmap::calendar::CalendarSyncEngine>(
            m_databaseConnection, *m_calendarProtocolClient);
        m_calendarMutationEngine =
            std::make_unique<javelin::jmap::calendar::CalendarMutationEngine>(
                m_databaseConnection, *m_calendarProtocolClient, *m_calendarSyncEngine,
                *m_calendarReader);
        m_sieveProtocolClient = std::make_unique<javelin::jmap::sieve::SieveProtocolClient>(
            m_databaseConnection, *m_transport, *m_methodTransport);
        m_sieveMutationEngine = std::make_unique<javelin::jmap::sieve::SieveMutationEngine>(
            m_databaseConnection, *m_sieveProtocolClient);
        m_identityService = std::make_unique<javelin::jmap::identity::IdentityService>(
            m_databaseConnection, *m_methodTransport);
        m_identityRepository =
            std::make_unique<javelin::jmap::cache::IdentityRepository>(m_databaseConnection);
        m_mailboxRepository =
            std::make_unique<javelin::jmap::cache::MailboxReadRepository>(m_databaseConnection);
        m_mailTagRepository =
            std::make_unique<javelin::jmap::cache::MailTagReadRepository>(m_databaseConnection);
        m_mailboxStatisticsRepository =
            std::make_unique<javelin::jmap::cache::MailboxStatisticsReadRepository>(
                m_databaseConnection);
        m_messageViewService =
            std::make_unique<javelin::jmap::cache::MessageViewService>(m_databaseConnection);
        m_mailboxMessageRepository =
            std::make_unique<javelin::jmap::cache::MailboxMessageReadRepository>(
                m_databaseConnection);
        m_mailboxFilterRepository =
            std::make_unique<javelin::jmap::cache::MailboxFilterReadRepository>(
                m_databaseConnection);
        m_submissionRepository =
            std::make_unique<javelin::jmap::cache::SubmissionRepository>(m_databaseConnection);
        m_jmapComposeService = std::make_unique<javelin::jmap::submission::ComposeService>(
            m_databaseConnection, *m_transport, *m_methodTransport, *m_messageContentClient,
            *m_emailMutationEngine);
        m_deferredSendSubmitter =
            std::make_unique<ComposeDeferredSendSubmitter>(*m_jmapComposeService);
        m_errorCoordinator = std::make_unique<ApplicationErrorCoordinator>(*m_accountRepository);
        m_accountRuntimeManager = std::make_unique<AccountRuntimeManager>(
            m_databaseConnection, *m_sessionRefreshClient, *m_accountBootstrapClient,
            *m_methodTransport, *m_stateChangeNetworkAccessManager, *m_webSocketFailureCooldowns,
            *m_accountRepository, *m_mailboxRepository, *m_errorCoordinator, *m_workScheduler);
        m_mailQueryApplicationService = std::make_unique<MailQueryApplicationService>(
            m_databaseConnection, *m_mailQueryMaterializer, *m_contactRepository,
            *m_mailTagRepository, *m_mailboxStatisticsRepository, *m_mailboxMessageRepository,
            *m_mailboxFilterRepository, *m_accountRuntimeManager, *m_errorCoordinator,
            *m_workScheduler, *m_mailboxMaintenanceRegistry);
        m_mailMutationApplicationService = std::make_unique<MailMutationApplicationService>(
            m_databaseConnection, *m_emailMutationEngine, *m_mailboxMutationEngine,
            *m_mailQueryClient, *m_mailboxRepository, *m_mailTagRepository,
            *m_mailboxMessageRepository, *m_accountRuntimeManager, *m_errorCoordinator,
            *m_workScheduler, *m_mailboxMaintenanceRegistry, *m_undoManager);
        m_messageContentApplicationService = std::make_unique<MessageContentApplicationService>(
            m_databaseConnection, *m_messageContentClient, *m_accountRuntimeManager,
            *m_errorCoordinator, *m_workScheduler, *m_mailboxMaintenanceRegistry);
        m_mailNotificationService = std::make_unique<MailNotificationService>(m_databaseConnection);
        QObject::connect(m_accountRuntimeManager.get(),
                         &AccountRuntimeManager::notificationEventsCommitted,
                         m_mailNotificationService.get(), &MailNotificationService::accountChanged);
        m_contactApplicationService = std::make_unique<ContactApplicationService>(
            *m_contactRepository, *m_contactSyncEngine, *m_accountRuntimeManager,
            *m_errorCoordinator, *m_workScheduler, *m_undoManager);
        m_calendarApplicationService = std::make_unique<CalendarApplicationService>(
            m_databaseConnection, *m_calendarReader, *m_calendarProtocolClient,
            *m_calendarSyncEngine, *m_calendarMutationEngine, *m_accountRuntimeManager,
            *m_errorCoordinator, *m_workScheduler, *m_undoManager);
        m_sieveApplicationService = std::make_unique<SieveApplicationService>(
            *m_sieveProtocolClient, *m_sieveMutationEngine, *m_accountRuntimeManager,
            *m_errorCoordinator, *m_workScheduler, *m_undoManager);
        m_mailQueryApplicationService->setThreadMaterializationCoordinator(
            m_threadMaterializationCoordinator.get());
        m_mailMutationApplicationService->setThreadMaterializationCoordinator(
            m_threadMaterializationCoordinator.get());
        m_messageContentApplicationService->setThreadMaterializationCoordinator(
            m_threadMaterializationCoordinator.get());
        m_threadMembershipMaterializationWorker =
            std::make_unique<ThreadMembershipMaterializationWorker>(
                m_databaseConnection, *m_methodTransport, *m_accountRuntimeManager);
        m_threadMaterializationCoordinator->setWorker(
            m_threadMembershipMaterializationWorker.get());
        QObject::connect(m_threadMembershipMaterializationWorker.get(),
                         &ThreadMembershipMaterializationWorker::membershipCommitted,
                         m_mailQueryApplicationService.get(),
                         [this](QString accountId, const QStringList& threadIds)
                         {
                             m_mailQueryApplicationService->publishThreadMaterializationCommitted(
                                 std::move(accountId), threadIds);
                         });
        QObject::connect(m_threadMembershipMaterializationWorker.get(),
                         &ThreadMembershipMaterializationWorker::childEmailsCommitted,
                         m_mailQueryApplicationService.get(),
                         [this](QString accountId, const QStringList& threadIds, const QStringList&)
                         {
                             m_mailQueryApplicationService->publishThreadMaterializationCommitted(
                                 std::move(accountId), threadIds);
                         });
        m_developerMaintenanceService = std::make_unique<DeveloperMaintenanceService>(
            location.databasePath, location.vaultRootPath, *m_mailboxMaintenanceRegistry,
            *m_mailQueryApplicationService, *m_workScheduler,
            [this](const std::string_view accountId, const std::string_view mailboxId)
            { m_fullMailSyncService->requestMailboxResync(accountId, mailboxId); },
            [this](const std::string_view accountId)
            {
                if (const auto error = m_threadMaterializationCoordinator->restoreAccount(
                        accountId, WorkPriority::VisibleMaterialization))
                    qWarning().noquote()
                        << "Could not recover Thread materialization after mailbox "
                           "cache clear"
                        << QString::fromStdString(std::string{accountId}) << error->message;
            });
        m_mailTransferWorkService = std::make_unique<MailTransferWorkService>(
            m_databaseConnection, *m_transport, *m_methodTransport, *m_messageContentClient,
            *m_accountRuntimeManager, *m_mailTransferHistoryCoordinator, *m_workScheduler,
            [this](const MailTransferOperationRecord& operation)
            {
                m_mailQueryApplicationService->publishCacheChange({
                    .accountId = QString::fromStdString(operation.destinationAccountId),
                    .mailboxIds = {QString::fromStdString(operation.destinationMailboxId)},
                    .queryWindows = {},
                    .searchWindows = {},
                    .messageContentEmailIds = {},
                    .mailboxTreeChanged = false,
                    .emailObjectsChanged = false,
                    .optimisticProjection = false,
                    .contactsChanged = false,
                    .identitiesChanged = false,
                });
                if (operation.operation != MailTransferOperation::Move)
                    return;
                QStringList sourceMailboxIds;
                if (operation.sourceMailboxId.has_value())
                    sourceMailboxIds.push_back(QString::fromStdString(*operation.sourceMailboxId));
                m_mailQueryApplicationService->publishCacheChange({
                    .accountId = QString::fromStdString(operation.sourceAccountId),
                    .mailboxIds = std::move(sourceMailboxIds),
                    .queryWindows = {},
                    .searchWindows = {},
                    .messageContentEmailIds = {},
                    .mailboxTreeChanged = false,
                    .emailObjectsChanged = false,
                    .optimisticProjection = false,
                    .contactsChanged = false,
                    .identitiesChanged = false,
                });
                if (!operation.sourceMailboxId.has_value())
                    m_fullMailSyncService->requestCatchUp(operation.sourceAccountId);
            },
            m_accountRuntimeManager.get());
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::accountConfigured,
                         m_mailTransferWorkService.get(), [this](const QString&)
                         { m_mailTransferWorkService->restoreRecoverable(); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::sessionRefreshed,
                         m_mailTransferWorkService.get(), [this](const QString&)
                         { m_mailTransferWorkService->restoreRecoverable(); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::networkReachable,
                         m_mailTransferWorkService.get(),
                         &MailTransferWorkService::networkBecameReachable);
        QObject::connect(m_errorCoordinator.get(),
                         &ApplicationErrorCoordinator::authenticationPauseChanged,
                         m_mailTransferWorkService.get(),
                         [this](const QString&, const bool paused)
                         {
                             if (!paused)
                                 m_mailTransferWorkService->authenticationBecameAvailable();
                         });
        m_mailExportService = std::make_unique<MailExportService>(
            m_databaseConnection, *m_methodTransport, *m_mailQueryClient, *m_messageContentClient,
            *m_accountRuntimeManager, *m_workScheduler);
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::accountConfigured,
                         m_mailExportService.get(),
                         [this](const QString&) { m_mailExportService->restoreRecoverable(); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::sessionRefreshed,
                         m_mailExportService.get(),
                         [this](const QString&) { m_mailExportService->restoreRecoverable(); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::networkReachable,
                         m_mailExportService.get(), &MailExportService::networkBecameReachable);
        QObject::connect(m_errorCoordinator.get(),
                         &ApplicationErrorCoordinator::authenticationPauseChanged,
                         m_mailExportService.get(),
                         [this](const QString&, const bool paused)
                         {
                             if (!paused)
                                 m_mailExportService->authenticationBecameAvailable();
                         });
        m_mailImportService = std::make_unique<MailImportService>(
            m_databaseConnection, *m_transport, *m_methodTransport, *m_accountRuntimeManager,
            *m_workScheduler,
            [this](const std::string_view accountId, const std::string_view mailboxId)
            {
                if (!m_accountRuntimeManager->requestMailboxSynchronization(accountId, mailboxId))
                    qWarning().noquote() << "Could not queue post-import mailbox synchronization"
                                         << QString::fromStdString(std::string{accountId})
                                         << QString::fromStdString(std::string{mailboxId});
            });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::accountConfigured,
                         m_mailImportService.get(),
                         [this](const QString&) { m_mailImportService->restoreRecoverable(); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::sessionRefreshed,
                         m_mailImportService.get(),
                         [this](const QString&) { m_mailImportService->restoreRecoverable(); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::networkReachable,
                         m_mailImportService.get(), &MailImportService::networkBecameReachable);
        QObject::connect(m_errorCoordinator.get(),
                         &ApplicationErrorCoordinator::authenticationPauseChanged,
                         m_mailImportService.get(),
                         [this](const QString&, const bool paused)
                         {
                             if (!paused)
                                 m_mailImportService->authenticationBecameAvailable();
                         });
        m_mailTransferCommandService = std::make_unique<MailTransferCommandService>(
            m_databaseConnection, *m_transport, *m_methodTransport, *m_messageContentClient,
            *m_accountRuntimeManager, *m_mailTransferHistoryCoordinator,
            m_threadMaterializationCoordinator.get());
        m_mailTransferCommandService->setWorkService(m_mailTransferWorkService.get());
        m_mailCommandService = std::make_unique<MailCommandService>(
            *m_mailMutationApplicationService, *m_mailTransferCommandService);
        m_sieveCommandService = std::make_unique<SieveCommandService>(*m_sieveApplicationService);
        m_identityCommandService = std::make_unique<IdentityCommandService>(
            *m_identityService, *m_accountRepository, *m_accountRuntimeManager, *m_errorCoordinator,
            *m_workScheduler, *m_mailQueryApplicationService);
        const auto refreshIdentityAccount = [this](const QString& accountId)
        {
            auto task = m_identityCommandService->requestSenderIdentities(accountId.toStdString());
            QCoro::connect(std::move(task), m_accountRuntimeManager.get(), [](const auto&) {});
        };
        const auto refreshOwnedIdentityAccounts =
            [this, refreshIdentityAccount](const QString& ownerAccountId)
        {
            const auto accounts = m_accountRepository->listOwnedBy(ownerAccountId.toStdString());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&accounts))
            {
                qWarning().noquote()
                    << "Could not enumerate sender Identity accounts" << error->message;
                return;
            }
            for (const auto& account :
                 std::get<std::vector<javelin::jmap::cache::CachedAccount>>(accounts))
            {
                if (account.hasSubmissionCapability)
                    refreshIdentityAccount(QString::fromStdString(account.accountId));
            }
        };
        QObject::connect(m_accountRuntimeManager.get(),
                         &AccountRuntimeManager::senderIdentityStateChanged,
                         m_accountRuntimeManager.get(), refreshIdentityAccount);
        QObject::connect(m_accountRuntimeManager.get(),
                         &AccountRuntimeManager::sessionCapabilitiesChanged,
                         m_accountRuntimeManager.get(), refreshOwnedIdentityAccounts);
        m_accountRefreshCommandService = std::make_unique<AccountRefreshCommandService>(
            *m_accountRuntimeManager, *m_contactApplicationService);
        m_messageContentCommandService =
            std::make_unique<MessageContentCommandService>(*m_messageContentApplicationService);
        m_mailApplicationEventsService = std::make_unique<MailApplicationEventsService>(
            *m_accountRuntimeManager, *m_mailQueryApplicationService,
            *m_mailMutationApplicationService, *m_messageContentApplicationService,
            *m_contactApplicationService);
        m_commandDispatcher = std::make_unique<CommandDispatcher>(*m_accountRefreshCommandService);
        m_calendarCommandService =
            std::make_unique<CalendarCommandService>(*m_calendarApplicationService);
        m_deferredSendService = std::make_unique<DeferredSendService>(
            *m_deferredSendRepository, *m_deferredSendSubmitter, *m_accountRuntimeManager,
            *m_undoManager);
        m_undoManager->setExecutor(QStringLiteral("deferred_send"), m_deferredSendService.get());
        m_composeService = std::make_unique<ComposeService>(
            *m_jmapComposeService, *m_errorCoordinator, *m_workScheduler, *m_accountRuntimeManager,
            *m_mailQueryApplicationService, *m_undoManager, *m_deferredSendService);
        m_composeCommandService = std::make_unique<ComposeCommandService>(*m_composeService);
        m_draftHistoryExecutor =
            std::make_unique<javelin::app::undo::DraftHistoryExecutor>(*m_composeService);
        m_undoManager->setExecutor(QStringLiteral("draft"), m_draftHistoryExecutor.get());
        m_mailHistoryExecutor = std::make_unique<javelin::app::undo::MailHistoryExecutor>(
            *m_mailMutationApplicationService);
        m_undoManager->setExecutor(QStringLiteral("mail_patch"), m_mailHistoryExecutor.get());
        m_mailTransferHistoryService =
            std::make_unique<javelin::app::undo::MailTransferHistoryService>(
                m_databaseConnection, *m_transport, *m_methodTransport, *m_accountRuntimeManager);
        m_mailTransferHistoryExecutor =
            std::make_unique<javelin::app::undo::MailTransferHistoryExecutor>(
                *m_mailTransferHistoryService);
        m_undoManager->setExecutor(QStringLiteral("mail_transfer"),
                                   m_mailTransferHistoryExecutor.get());
        m_sieveHistoryExecutor =
            std::make_unique<javelin::app::undo::SieveHistoryExecutor>(*m_sieveApplicationService);
        m_undoManager->setExecutor(QStringLiteral("sieve"), m_sieveHistoryExecutor.get());
        m_calendarHistoryExecutor = std::make_unique<javelin::app::undo::CalendarHistoryExecutor>(
            *m_calendarApplicationService);
        m_undoManager->setExecutor(QStringLiteral("calendar_event"),
                                   m_calendarHistoryExecutor.get());
        m_calendarPreferenceExecutor =
            std::make_unique<javelin::app::undo::CalendarPreferenceExecutor>(
                *m_calendarApplicationService);
        m_undoManager->setExecutor(QStringLiteral("calendar_preference"),
                                   m_calendarPreferenceExecutor.get());
        m_contactCommandService = std::make_unique<ContactCommandService>(
            *m_accountRuntimeManager, *m_contactSyncEngine, *m_contactMutationEngine,
            *m_contactMediaService, *m_contactRepository, *m_errorCoordinator, *m_workScheduler,
            *m_undoManager);
        m_contactHistoryExecutor =
            std::make_unique<javelin::app::undo::ContactHistoryExecutor>(*m_contactCommandService);
        m_undoManager->setExecutor(QStringLiteral("contact_card"), m_contactHistoryExecutor.get());
        m_addressBookHistoryExecutor =
            std::make_unique<javelin::app::undo::AddressBookHistoryExecutor>(
                *m_contactCommandService, *m_contactCommandService);
        m_undoManager->setExecutor(QStringLiteral("address_book"),
                                   m_addressBookHistoryExecutor.get());
        QObject::connect(
            m_fullMailSyncService.get(), &FullMailSyncService::mailboxWindowCommitted,
            m_mailQueryApplicationService.get(),
            [this](QString accountId, QString mailboxId, const quint64 offset, const quint64 limit)
            {
                m_mailQueryApplicationService->publishMailboxWindowCommitted(
                    std::move(accountId), std::move(mailboxId), static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(limit));
            });
        QObject::connect(m_fullMailSyncService.get(), &FullMailSyncService::messageContentCommitted,
                         m_messageContentApplicationService.get(),
                         &MessageContentApplicationService::publishMessageContentCommitted);
        const auto refreshMailboxVisibility = [this](const MailCacheChange& change)
        {
            if (change.mailboxTreeChanged)
                m_fullMailSyncService->refreshMailboxVisibility(change.accountId.toStdString());
        };
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::cacheCommitted,
                         m_fullMailSyncService.get(), refreshMailboxVisibility);
        QObject::connect(m_mailMutationApplicationService.get(),
                         &MailMutationApplicationService::cacheCommitted,
                         m_fullMailSyncService.get(), refreshMailboxVisibility);
        m_messageNavigationCoordinator = std::make_unique<MessageNavigationCoordinator>();
        m_calendarNotificationService = std::make_unique<CalendarNotificationService>(
            m_databaseConnection, *m_calendarApplicationService, *m_calendarApplicationService);
        m_calendarInvitationService = std::make_unique<CalendarInvitationService>(
            m_databaseConnection, *m_calendarProtocolClient, *m_calendarReader,
            *m_accountRuntimeManager);
        QObject::connect(
            m_calendarApplicationService.get(), &CalendarApplicationService::calendarMetadataReady,
            m_calendarInvitationService.get(), &CalendarInvitationService::accountChanged);
        QObject::connect(m_calendarApplicationService.get(),
                         &CalendarApplicationService::calendarMetadataReady,
                         m_calendarNotificationService.get(),
                         &CalendarNotificationService::calendarMetadataReady);
        QObject::connect(m_calendarNotificationService.get(),
                         &CalendarNotificationService::calendarMetadataRequired,
                         m_calendarApplicationService.get(),
                         [this](const QString& ownerAccountId)
                         {
                             m_calendarApplicationService->ensureCalendarMetadata(
                                 ownerAccountId.toStdString());
                         });
        QObject::connect(
            m_accountRuntimeManager.get(), &AccountRuntimeManager::calendarStateChanged,
            m_calendarInvitationService.get(), &CalendarInvitationService::calendarStateChanged);
        QObject::connect(
            m_accountRuntimeManager.get(), &AccountRuntimeManager::calendarStateChanged,
            m_calendarNotificationService.get(),
            [this](const QString& ownerAccountId,
                   const javelin::jmap::sync::AccountTypeStateMap& changedStates)
            {
                if (std::ranges::any_of(changedStates, [](const auto& account)
                                        { return account.second.contains("CalendarEvent"); }))
                    m_calendarNotificationService->calendarStateChanged(ownerAccountId);
            });
        QObject::connect(
            m_accountRuntimeManager.get(), &AccountRuntimeManager::stateChangeCatchUpRequired,
            m_calendarInvitationService.get(), &CalendarInvitationService::accountChanged);
        QObject::connect(m_accountRuntimeManager.get(),
                         &AccountRuntimeManager::calendarAlertReceived,
                         m_calendarNotificationService.get(),
                         &CalendarNotificationService::calendarAlertReceived);
        QObject::connect(
            m_calendarApplicationService.get(), &CalendarApplicationService::calendarCacheCommitted,
            m_calendarNotificationService.get(), [this](const CalendarCacheChange& change)
            { m_calendarNotificationService->calendarCacheCommitted(change.ownerAccountId); });
        QObject::connect(m_accountRuntimeManager.get(), &AccountRuntimeManager::accountRemoved,
                         m_calendarNotificationService.get(),
                         &CalendarNotificationService::calendarAccountRemoved);
        QObject::connect(m_calendarApplicationService.get(),
                         &CalendarApplicationService::calendarCacheCommitted,
                         m_calendarInvitationService.get(), [this](const CalendarCacheChange&)
                         { m_calendarInvitationService->calendarCacheCommitted(); });
    }

    DaemonServices::~DaemonServices() = default;

    javelin::jmap::auth::AccountOnboardingService& DaemonServices::onboardingService()
    {
        return *m_onboardingService;
    }

    void DaemonServices::setAccessTokenProvider(javelin::jmap::auth::AccessTokenProvider provider)
    {
        m_transport->setAccessTokenProvider(provider);
        m_methodTransport->setAccessTokenProvider(std::move(provider));
    }

    void DaemonServices::setAuthenticationRefreshHandler(
        javelin::jmap::auth::AccessTokenRefreshHandler handler)
    {
        m_transport->setRefreshHandler(handler);
        m_methodTransport->setRefreshHandler(handler);
        m_accountRuntimeManager->setAuthenticationRefreshHandler(std::move(handler));
    }

    javelin::jmap::cache::AccountRepository& DaemonServices::accountRepository()
    {
        return *m_accountRepository;
    }

    AccountCommandPort& DaemonServices::accountCommandPort()
    {
        return *m_accountCommandService;
    }

    javelin::jmap::cache::DatabaseConnection& DaemonServices::databaseConnection()
    {
        return m_databaseConnection;
    }

    const QString& DaemonServices::databasePath() const
    {
        return m_databasePath;
    }

    javelin::protocol::CacheIdentity DaemonServices::cacheIdentity() const
    {
        const auto dataVersion = m_databaseConnection.dataVersion();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&dataVersion))
            throw std::runtime_error(error->message.toStdString());
        return {
            .instance = {.value = m_cacheInstanceId},
            .schema = {.value = static_cast<std::uint32_t>(m_databaseConnection.schemaVersion())},
            .dataVersion = {.value = std::get<std::uint64_t>(dataVersion)}};
    }

    CacheAccessBarrier& DaemonServices::cacheAccessBarrier()
    {
        return *m_cacheAccessBarrier;
    }

    javelin::jmap::cache::ContactRepository& DaemonServices::contactRepository()
    {
        return *m_contactRepository;
    }

    javelin::jmap::calendar::CalendarReader& DaemonServices::calendarReader()
    {
        return *m_calendarReader;
    }

    CalendarCommandPort& DaemonServices::calendarCommandPort()
    {
        return *m_calendarCommandService;
    }

    javelin::jmap::cache::IdentityRepository& DaemonServices::identityRepository()
    {
        return *m_identityRepository;
    }

    javelin::jmap::cache::MessageViewService& DaemonServices::messageViewService()
    {
        return *m_messageViewService;
    }

    javelin::jmap::cache::MailboxReader& DaemonServices::mailboxReader()
    {
        return *m_mailboxRepository;
    }

    ComposeService& DaemonServices::composeService()
    {
        return *m_composeService;
    }

    ComposeCommandPort& DaemonServices::composeCommandPort()
    {
        return *m_composeCommandService;
    }

    MailCommandPort& DaemonServices::mailCommandPort()
    {
        return *m_mailCommandService;
    }

    MailExportPort& DaemonServices::mailExportPort()
    {
        return *m_mailExportService;
    }

    MailImportPort& DaemonServices::mailImportPort()
    {
        return *m_mailImportService;
    }

    SieveCommandPort& DaemonServices::sieveCommandPort()
    {
        return *m_sieveCommandService;
    }

    IdentityCommandPort& DaemonServices::identityCommandPort()
    {
        return *m_identityCommandService;
    }

    AccountRefreshPort& DaemonServices::accountRefreshPort()
    {
        return *m_accountRefreshCommandService;
    }

    MessageContentPort& DaemonServices::messageContentPort()
    {
        return *m_messageContentCommandService;
    }

    MailApplicationEventsPort& DaemonServices::mailApplicationEvents()
    {
        return *m_mailApplicationEventsService;
    }

    CommandDispatcher& DaemonServices::commandDispatcher()
    {
        return *m_commandDispatcher;
    }

    UndoCommandPort& DaemonServices::undoCommandPort()
    {
        return *m_undoCommandService;
    }

    DeferredSendService& DaemonServices::deferredSendService()
    {
        return *m_deferredSendService;
    }

    ContactCommandPort& DaemonServices::contactCommandPort()
    {
        return *m_contactCommandService;
    }

    AccountRuntimeManager& DaemonServices::accountRuntimeManager()
    {
        return *m_accountRuntimeManager;
    }

    MailQueryApplicationService& DaemonServices::mailQueryApplicationService()
    {
        return *m_mailQueryApplicationService;
    }

    MailMutationApplicationService& DaemonServices::mailMutationApplicationService()
    {
        return *m_mailMutationApplicationService;
    }

    MailNotificationService& DaemonServices::mailNotificationService()
    {
        return *m_mailNotificationService;
    }

    CalendarApplicationService& DaemonServices::calendarApplicationService()
    {
        return *m_calendarApplicationService;
    }

    SieveApplicationService& DaemonServices::sieveApplicationService()
    {
        return *m_sieveApplicationService;
    }

    MessageNavigationPort& DaemonServices::messageNavigationPort()
    {
        return *m_messageNavigationCoordinator;
    }

    ApplicationErrorCoordinator& DaemonServices::errorCoordinator()
    {
        return *m_errorCoordinator;
    }

    CalendarNotificationService& DaemonServices::calendarNotificationService()
    {
        return *m_calendarNotificationService;
    }

    CalendarInvitationService& DaemonServices::calendarInvitationService()
    {
        return *m_calendarInvitationService;
    }

    WorkScheduler& DaemonServices::workScheduler()
    {
        return *m_workScheduler;
    }

    ThreadMaterializationCoordinator& DaemonServices::threadMaterializationCoordinator()
    {
        return *m_threadMaterializationCoordinator;
    }

    LocalMaintenanceService& DaemonServices::localMaintenanceService()
    {
        return *m_localMaintenanceService;
    }

    DeveloperDiagnosticsPort& DaemonServices::developerDiagnosticsPort()
    {
        return *m_developerDiagnosticsService;
    }

    DeveloperMaintenancePort& DaemonServices::developerMaintenancePort()
    {
        return *m_developerMaintenanceService;
    }

    FullMailSyncService& DaemonServices::fullMailSyncService()
    {
        return *m_fullMailSyncService;
    }

    MailIndexService& DaemonServices::mailIndexService()
    {
        return *m_mailIndexService;
    }

    javelin::app::undo::UndoManager& DaemonServices::undoManager()
    {
        return *m_undoManager;
    }

} // namespace javelin::app
