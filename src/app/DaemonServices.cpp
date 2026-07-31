#include "app/DaemonServices.h"

#include "app/AccountCommandService.h"
#include "app/AccountRefreshCommandService.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/CacheAccessBarrier.h"
#include "app/CacheLocationProvider.h"
#include "app/CalendarCommandService.h"
#include "app/CalendarNotificationService.h"
#include "app/CommandDispatcher.h"
#include "app/ComposeCommandService.h"
#include "app/ComposeService.h"
#include "app/ContactCommandService.h"
#include "app/DeferredSendRepository.h"
#include "app/DeferredSendService.h"
#include "app/FullMailSyncService.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationEventsService.h"
#include "app/MailApplicationService.h"
#include "app/MailCommandService.h"
#include "app/MailIndexService.h"
#include "app/MessageContentCommandService.h"
#include "app/MessageListSessionFactoryService.h"
#include "app/MessageNavigationCoordinator.h"
#include "app/SieveCommandService.h"
#include "app/TranslationService.h"
#include "app/UndoCommandService.h"
#include "app/WorkScheduler.h"
#include "app/undo/AddressBookHistoryExecutor.h"
#include "app/undo/CalendarHistoryExecutor.h"
#include "app/undo/CalendarPreferenceExecutor.h"
#include "app/undo/ContactHistoryExecutor.h"
#include "app/undo/DraftHistoryExecutor.h"
#include "app/undo/HistoryRepository.h"
#include "app/undo/MailHistoryExecutor.h"
#include "app/undo/SieveHistoryExecutor.h"
#include "app/undo/UndoManager.h"

#include "jmap/JmapCore.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SubmissionRepository.h"
#include "jmap/cache/TranslationCacheRepository.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/sieve/SieveService.h"
#include "jmap/submission/ComposeService.h"
#include "jmap/sync/MutationJournal.h"

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

    DaemonServices::DaemonServices()
    {
        const auto location = cacheLocation();
        auto databaseResult = javelin::jmap::cache::DaemonDatabaseFactory{
            javelin::jmap::cache::DatabaseConnectionOptions{
                .connectionName = QStringLiteral("javelin-gui-main"),
                .databasePath = location.databasePath,
            }}.open();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&databaseResult))
        {
            throw std::runtime_error(error->message.toStdString());
        }

        m_databaseConnection =
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(databaseResult));
        m_databasePath = location.databasePath;
        m_cacheAccessBarrier = std::make_unique<CacheAccessBarrier>();
        m_workScheduler = std::make_unique<WorkScheduler>(m_databaseConnection);
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
        m_networkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_stateChangeNetworkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_webSocketFailureCooldowns =
            std::make_unique<javelin::jmap::api::WebSocketFailureCooldowns>();
        m_transport =
            std::make_unique<javelin::jmap::api::QtNetworkTransport>(*m_networkAccessManager);
        m_httpMethodTransport =
            std::make_unique<javelin::jmap::api::HttpJmapMethodTransport>(*m_transport);
        m_methodTransport = std::make_unique<javelin::jmap::api::PreferredJmapMethodTransport>(
            m_databaseConnection, *m_httpMethodTransport, *m_webSocketFailureCooldowns);
        m_jmapCore = std::make_unique<javelin::jmap::JmapCore>(m_databaseConnection, *m_transport,
                                                               *m_methodTransport);
        m_mailIndexService =
            std::make_unique<MailIndexService>(m_databaseConnection, *m_workScheduler);
        m_fullMailSyncService = std::make_unique<FullMailSyncService>(
            m_databaseConnection, *m_jmapCore, *m_workScheduler, *m_mailIndexService);
        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountRepository>(m_databaseConnection);
        m_accountCommandService = std::make_unique<AccountCommandService>(*m_accountRepository);
        m_contactRepository =
            std::make_unique<javelin::jmap::cache::ContactRepository>(m_databaseConnection);
        m_contactService = std::make_unique<javelin::jmap::contacts::ContactService>(
            m_databaseConnection, *m_contactRepository, *m_transport, *m_methodTransport);
        m_calendarService = std::make_unique<javelin::jmap::calendar::CalendarService>(
            m_databaseConnection, *m_methodTransport);
        m_sieveService = std::make_unique<javelin::jmap::sieve::SieveService>(
            m_databaseConnection, *m_transport, *m_methodTransport);
        m_identityRepository =
            std::make_unique<javelin::jmap::cache::IdentityRepository>(m_databaseConnection);
        m_messageViewService =
            std::make_unique<javelin::jmap::cache::MessageViewService>(m_databaseConnection);
        m_queryService = std::make_unique<javelin::jmap::cache::QueryService>(m_databaseConnection);
        m_translationCacheRepository =
            std::make_unique<javelin::jmap::cache::TranslationCacheRepository>(
                m_databaseConnection);
        m_translationService = std::make_unique<TranslationService>(*m_networkAccessManager,
                                                                    *m_translationCacheRepository);
        m_submissionRepository =
            std::make_unique<javelin::jmap::cache::SubmissionRepository>(m_databaseConnection);
        m_jmapComposeService = std::make_unique<javelin::jmap::submission::ComposeService>(
            m_databaseConnection, *m_transport, *m_methodTransport, *m_jmapCore);
        m_errorCoordinator = std::make_unique<ApplicationErrorCoordinator>();
        m_mailService = std::make_unique<MailApplicationService>(
            m_databaseConnection, *m_jmapCore, *m_methodTransport,
            *m_stateChangeNetworkAccessManager, *m_webSocketFailureCooldowns, *m_accountRepository,
            *m_queryService, *m_contactService, *m_calendarService, *m_sieveService,
            *m_errorCoordinator, *m_workScheduler, *m_undoManager);
        m_mailCommandService = std::make_unique<MailCommandService>(*m_mailService);
        m_sieveCommandService = std::make_unique<SieveCommandService>(*m_mailService);
        m_accountRefreshCommandService =
            std::make_unique<AccountRefreshCommandService>(*m_mailService);
        m_messageContentCommandService =
            std::make_unique<MessageContentCommandService>(*m_mailService);
        m_mailApplicationEventsService =
            std::make_unique<MailApplicationEventsService>(*m_mailService);
        m_messageListSessionFactoryService = std::make_unique<MessageListSessionFactoryService>(
            *m_mailService, *m_mailApplicationEventsService);
        m_commandDispatcher = std::make_unique<CommandDispatcher>(*m_accountRefreshCommandService);
        m_calendarCommandService = std::make_unique<CalendarCommandService>(*m_mailService);
        m_deferredSendService = std::make_unique<DeferredSendService>(
            *m_deferredSendRepository, *m_jmapComposeService, *m_mailService, *m_undoManager);
        m_undoManager->setExecutor(QStringLiteral("deferred_send"), m_deferredSendService.get());
        m_composeService = std::make_unique<ComposeService>(
            *m_jmapComposeService, *m_errorCoordinator, *m_workScheduler, *m_mailService,
            *m_undoManager, *m_deferredSendService);
        m_composeCommandService = std::make_unique<ComposeCommandService>(*m_composeService);
        m_draftHistoryExecutor =
            std::make_unique<javelin::app::undo::DraftHistoryExecutor>(*m_composeService);
        m_undoManager->setExecutor(QStringLiteral("draft"), m_draftHistoryExecutor.get());
        m_mailHistoryExecutor =
            std::make_unique<javelin::app::undo::MailHistoryExecutor>(*m_mailService);
        m_undoManager->setExecutor(QStringLiteral("mail_patch"), m_mailHistoryExecutor.get());
        m_sieveHistoryExecutor =
            std::make_unique<javelin::app::undo::SieveHistoryExecutor>(*m_mailService);
        m_undoManager->setExecutor(QStringLiteral("sieve"), m_sieveHistoryExecutor.get());
        m_calendarHistoryExecutor =
            std::make_unique<javelin::app::undo::CalendarHistoryExecutor>(*m_mailService);
        m_undoManager->setExecutor(QStringLiteral("calendar_event"),
                                   m_calendarHistoryExecutor.get());
        m_calendarPreferenceExecutor =
            std::make_unique<javelin::app::undo::CalendarPreferenceExecutor>(*m_mailService);
        m_undoManager->setExecutor(QStringLiteral("calendar_preference"),
                                   m_calendarPreferenceExecutor.get());
        m_contactCommandService = std::make_unique<ContactCommandService>(
            *m_mailService, *m_contactService, *m_contactRepository, *m_errorCoordinator,
            *m_workScheduler, *m_undoManager);
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
            m_mailService.get(),
            [this](QString accountId, QString mailboxId, const quint64 offset, const quint64 limit)
            {
                m_mailService->publishMailboxWindowCommitted(
                    std::move(accountId), std::move(mailboxId), static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(limit));
            });
        m_messageNavigationCoordinator = std::make_unique<MessageNavigationCoordinator>();
        m_calendarNotificationService =
            std::make_unique<CalendarNotificationService>(m_databaseConnection, *m_mailService);
        QObject::connect(m_mailService.get(), &MailApplicationService::calendarCacheCommitted,
                         m_calendarNotificationService.get(), [this](const CalendarCacheChange&)
                         { m_calendarNotificationService->requestScan(); });
    }

    DaemonServices::~DaemonServices() = default;

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

    CacheAccessBarrier& DaemonServices::cacheAccessBarrier()
    {
        return *m_cacheAccessBarrier;
    }

    javelin::jmap::cache::ContactRepository& DaemonServices::contactRepository()
    {
        return *m_contactRepository;
    }

    javelin::jmap::contacts::ContactService& DaemonServices::contactService()
    {
        return *m_contactService;
    }

    javelin::jmap::calendar::CalendarService& DaemonServices::calendarService()
    {
        return *m_calendarService;
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

    javelin::jmap::cache::QueryService& DaemonServices::queryService()
    {
        return *m_queryService;
    }

    TranslationPort& DaemonServices::translationService()
    {
        return *m_translationService;
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

    SieveCommandPort& DaemonServices::sieveCommandPort()
    {
        return *m_sieveCommandService;
    }

    AccountRefreshPort& DaemonServices::accountRefreshPort()
    {
        return *m_accountRefreshCommandService;
    }

    MessageContentPort& DaemonServices::messageContentPort()
    {
        return *m_messageContentCommandService;
    }

    MessageListSessionFactoryPort& DaemonServices::messageListSessionFactory()
    {
        return *m_messageListSessionFactoryService;
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

    MailApplicationService& DaemonServices::mailService()
    {
        return *m_mailService;
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

    WorkScheduler& DaemonServices::workScheduler()
    {
        return *m_workScheduler;
    }

    LocalMaintenanceService& DaemonServices::localMaintenanceService()
    {
        return *m_localMaintenanceService;
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
