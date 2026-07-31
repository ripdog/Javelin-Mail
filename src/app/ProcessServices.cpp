#include "app/ProcessServices.h"

#include "app/AddressSuggestionStore.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/CacheLocationProvider.h"
#include "app/CalendarNotificationService.h"
#include "app/ComposeService.h"
#include "app/ContactCommandService.h"
#include "app/DeferredSendRepository.h"
#include "app/DeferredSendService.h"
#include "app/FullMailSyncService.h"
#include "app/InlineMessageSchemeHandler.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationService.h"
#include "app/MailIndexService.h"
#include "app/MessageNavigationCoordinator.h"
#include "app/TranslationService.h"
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
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SubmissionRepository.h"
#include "jmap/cache/TranslationCacheRepository.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/render/InlineMessageUrl.h"
#include "jmap/sieve/SieveService.h"
#include "jmap/submission/ComposeService.h"
#include "jmap/sync/MutationJournal.h"

#include <QWebEngineProfile>

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

    ProcessServices::ProcessServices(const bool installInlineMessageSchemeHandler)
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
        auto guiDatabaseResult = javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
                .databasePath = location.databasePath,
            }}.openForCurrentThread("main");
        if (const auto* error =
                std::get_if<javelin::jmap::cache::DatabaseError>(&guiDatabaseResult))
        {
            throw std::runtime_error(error->message.toStdString());
        }
        m_guiReadDatabaseConnection = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(
            std::move(guiDatabaseResult));
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
        if (installInlineMessageSchemeHandler)
        {
            m_inlineMessageSchemeHandler =
                std::make_unique<InlineMessageSchemeHandler>(m_databaseConnection);
            QWebEngineProfile::defaultProfile()->installUrlSchemeHandler(
                javelin::jmap::render::inlineMessageUrlScheme().toUtf8(),
                m_inlineMessageSchemeHandler.get());
        }
        m_jmapCore = std::make_unique<javelin::jmap::JmapCore>(m_databaseConnection, *m_transport,
                                                               *m_methodTransport);
        m_mailIndexService =
            std::make_unique<MailIndexService>(m_databaseConnection, *m_workScheduler);
        m_fullMailSyncService = std::make_unique<FullMailSyncService>(
            m_databaseConnection, *m_jmapCore, *m_workScheduler, *m_mailIndexService);
        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountRepository>(m_databaseConnection);
        m_accountReadRepository = std::make_unique<javelin::jmap::cache::AccountReadRepository>(
            m_guiReadDatabaseConnection);
        m_mailboxReadRepository = std::make_unique<javelin::jmap::cache::MailboxReadRepository>(
            m_guiReadDatabaseConnection);
        m_contactRepository =
            std::make_unique<javelin::jmap::cache::ContactRepository>(m_databaseConnection);
        AddressSuggestionStore::instance().initialize(m_databaseConnection);
        QObject::connect(m_contactRepository.get(),
                         &javelin::jmap::cache::ContactRepository::contactsChanged,
                         &AddressSuggestionStore::instance(), &AddressSuggestionStore::refresh);
        m_contactService = std::make_unique<javelin::jmap::contacts::ContactService>(
            m_databaseConnection, *m_contactRepository, *m_transport, *m_methodTransport);
        m_calendarService = std::make_unique<javelin::jmap::calendar::CalendarService>(
            m_databaseConnection, *m_methodTransport);
        m_sieveService = std::make_unique<javelin::jmap::sieve::SieveService>(
            m_databaseConnection, *m_transport, *m_methodTransport);
        m_contactIdentityLookup =
            std::make_unique<javelin::jmap::contacts::ContactIdentityLookup>(*m_contactRepository);
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
        m_deferredSendService = std::make_unique<DeferredSendService>(
            *m_deferredSendRepository, *m_jmapComposeService, *m_mailService, *m_undoManager);
        m_undoManager->setExecutor(QStringLiteral("deferred_send"), m_deferredSendService.get());
        m_composeService = std::make_unique<ComposeService>(
            *m_jmapComposeService, *m_errorCoordinator, *m_workScheduler, *m_mailService,
            *m_undoManager, *m_deferredSendService);
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

    ProcessServices::~ProcessServices() = default;

    javelin::jmap::cache::AccountRepository& ProcessServices::accountRepository()
    {
        return *m_accountRepository;
    }

    javelin::jmap::cache::AccountReader& ProcessServices::accountReader()
    {
        return *m_accountReadRepository;
    }

    javelin::jmap::cache::MailboxReader& ProcessServices::mailboxReader()
    {
        return *m_mailboxReadRepository;
    }

    javelin::jmap::cache::DatabaseConnection& ProcessServices::databaseConnection()
    {
        return m_databaseConnection;
    }

    javelin::jmap::cache::ContactRepository& ProcessServices::contactRepository()
    {
        return *m_contactRepository;
    }

    javelin::jmap::contacts::ContactService& ProcessServices::contactService()
    {
        return *m_contactService;
    }

    javelin::jmap::calendar::CalendarService& ProcessServices::calendarService()
    {
        return *m_calendarService;
    }

    javelin::jmap::contacts::ContactIdentityLookup& ProcessServices::contactIdentityLookup()
    {
        return *m_contactIdentityLookup;
    }

    javelin::jmap::cache::IdentityRepository& ProcessServices::identityRepository()
    {
        return *m_identityRepository;
    }

    javelin::jmap::cache::MessageViewService& ProcessServices::messageViewService()
    {
        return *m_messageViewService;
    }

    javelin::jmap::cache::QueryService& ProcessServices::queryService()
    {
        return *m_queryService;
    }

    TranslationService& ProcessServices::translationService()
    {
        return *m_translationService;
    }

    ComposeService& ProcessServices::composeService()
    {
        return *m_composeService;
    }

    DeferredSendService& ProcessServices::deferredSendService()
    {
        return *m_deferredSendService;
    }

    ContactCommandPort& ProcessServices::contactCommandPort()
    {
        return *m_contactCommandService;
    }

    MailApplicationService& ProcessServices::mailService()
    {
        return *m_mailService;
    }

    MessageNavigationCoordinator& ProcessServices::messageNavigationCoordinator()
    {
        return *m_messageNavigationCoordinator;
    }

    ApplicationErrorCoordinator& ProcessServices::errorCoordinator()
    {
        return *m_errorCoordinator;
    }

    CalendarNotificationService& ProcessServices::calendarNotificationService()
    {
        return *m_calendarNotificationService;
    }

    WorkScheduler& ProcessServices::workScheduler()
    {
        return *m_workScheduler;
    }

    LocalMaintenanceService& ProcessServices::localMaintenanceService()
    {
        return *m_localMaintenanceService;
    }

    FullMailSyncService& ProcessServices::fullMailSyncService()
    {
        return *m_fullMailSyncService;
    }

    MailIndexService& ProcessServices::mailIndexService()
    {
        return *m_mailIndexService;
    }

    javelin::app::undo::UndoManager& ProcessServices::undoManager()
    {
        return *m_undoManager;
    }

} // namespace javelin::app
