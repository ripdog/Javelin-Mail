#pragma once

#include "app/CacheLocationProvider.h"
#include "jmap/auth/Auth.h"
#include "protocol/CacheContract.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <memory>

class QNetworkAccessManager;

namespace javelin::jmap
{
    class AccountBootstrapClient;
    class EmailMutationEngine;
    class MailboxMutationEngine;
    class MailQueryClient;
    class MailQueryMaterializer;
    class MessageContentClient;
    class SessionRefreshClient;
} // namespace javelin::jmap

namespace javelin::jmap::auth
{
    class AccountOnboardingService;
}

namespace javelin::jmap::contacts
{
    class ContactMediaService;
    class ContactMutationEngine;
    class ContactProtocolClient;
    class ContactSyncEngine;
} // namespace javelin::jmap::contacts

namespace javelin::app
{
    class WorkScheduler;
    class CommandDispatcher;
    class CacheAccessBarrier;
    class FullMailSyncService;
    class MailIndexService;
    class LocalMaintenanceService;
    class ApplicationErrorCoordinator;
    class ComposeService;
    class ComposeCommandPort;
    class ComposeCommandService;
    class MailCommandPort;
    class MailCommandService;
    class MailExportPort;
    class MailExportService;
    class MailTransferCommandService;
    class MailTransferWorkService;
    class SieveCommandPort;
    class SieveCommandService;
    class IdentityCommandPort;
    class IdentityCommandService;
    class AccountRefreshPort;
    class AccountRefreshCommandService;
    class MessageContentPort;
    class MessageContentCommandService;
    class MessageListSessionFactoryPort;
    class MessageListSessionFactoryService;
    class MailApplicationEventsPort;
    class MailApplicationEventsService;
    class UndoCommandPort;
    class UndoCommandService;
    class DeferredSendRepository;
    class DeferredSendSubmitter;
    class DeferredSendService;
    class DeveloperDiagnosticsPort;
    class DeveloperDiagnosticsService;
    class DeveloperMaintenancePort;
    class DeveloperMaintenanceService;
    class MailboxMaintenanceRegistry;
    class AccountCommandPort;
    class AccountCommandService;
    class ContactCommandPort;
    class ContactCommandService;
    class CalendarNotificationService;
    class CalendarInvitationService;
    class CalendarCommandPort;
    class CalendarCommandService;
    class MessageNavigationPort;
    class MessageNavigationCoordinator;
    class AccountRuntimeManager;
    class CalendarApplicationService;
    class ContactApplicationService;
    class MailMutationApplicationService;
    class MailNotificationService;
    class MailQueryApplicationService;
    class MessageContentApplicationService;
    class SieveApplicationService;
    class ThreadMaterializationCoordinator;
    class ThreadMembershipMaterializationWorker;
} // namespace javelin::app

namespace javelin::app::undo
{
    class AddressBookHistoryExecutor;
    class HistoryRepository;
    class DraftHistoryExecutor;
    class CalendarHistoryExecutor;
    class CalendarPreferenceExecutor;
    class ContactHistoryExecutor;
    class MailHistoryExecutor;
    class MailTransferHistoryCoordinator;
    class MailTransferHistoryExecutor;
    class MailTransferHistoryService;
    class SieveHistoryExecutor;
    class UndoManager;
} // namespace javelin::app::undo

namespace javelin::jmap::api
{
    class HttpJmapMethodTransport;
    class PreferredJmapMethodTransport;
    class RefreshingJmapMethodTransport;
    class RefreshingTransport;
    class QtNetworkTransport;
    class WebSocketFailureCooldowns;
} // namespace javelin::jmap::api

namespace javelin::jmap::cache
{
    class AccountRepository;
    class ContactRepository;
    class IdentityRepository;
    class MailboxFilterReadRepository;
    class MailboxMessageReadRepository;
    class MailboxReadRepository;
    class MailboxReader;
    class MailTagReadRepository;
    class MailboxStatisticsReadRepository;
    class MessageViewService;
    class SubmissionRepository;
} // namespace javelin::jmap::cache

namespace javelin::jmap::submission
{
    class ComposeService;
}
namespace javelin::jmap::calendar
{
    class CalendarCacheReader;
    class CalendarMutationEngine;
    class CalendarProtocolClient;
    class CalendarReader;
    class CalendarSyncEngine;
} // namespace javelin::jmap::calendar
namespace javelin::jmap::sieve
{
    class SieveMutationEngine;
    class SieveProtocolClient;
} // namespace javelin::jmap::sieve
namespace javelin::jmap::identity
{
    class IdentityService;
}

namespace javelin::app
{

    class DaemonServices
    {
      public:
        DaemonServices();
        explicit DaemonServices(CacheLocation location);
        ~DaemonServices();

        DaemonServices(const DaemonServices&) = delete;
        DaemonServices& operator=(const DaemonServices&) = delete;
        DaemonServices(DaemonServices&&) = delete;
        DaemonServices& operator=(DaemonServices&&) = delete;

        [[nodiscard]] javelin::jmap::cache::AccountRepository& accountRepository();
        [[nodiscard]] AccountCommandPort& accountCommandPort();
        [[nodiscard]] javelin::jmap::cache::DatabaseConnection& databaseConnection();
        [[nodiscard]] const QString& databasePath() const;
        [[nodiscard]] javelin::protocol::CacheIdentity cacheIdentity() const;
        [[nodiscard]] CacheAccessBarrier& cacheAccessBarrier();
        [[nodiscard]] javelin::jmap::cache::ContactRepository& contactRepository();
        [[nodiscard]] javelin::jmap::calendar::CalendarReader& calendarReader();
        [[nodiscard]] CalendarCommandPort& calendarCommandPort();
        [[nodiscard]] javelin::jmap::cache::IdentityRepository& identityRepository();
        [[nodiscard]] javelin::jmap::cache::MessageViewService& messageViewService();
        [[nodiscard]] javelin::jmap::cache::MailboxReader& mailboxReader();
        [[nodiscard]] ComposeService& composeService();
        [[nodiscard]] ComposeCommandPort& composeCommandPort();
        [[nodiscard]] MailCommandPort& mailCommandPort();
        [[nodiscard]] MailExportPort& mailExportPort();
        [[nodiscard]] SieveCommandPort& sieveCommandPort();
        [[nodiscard]] IdentityCommandPort& identityCommandPort();
        [[nodiscard]] AccountRefreshPort& accountRefreshPort();
        [[nodiscard]] MessageContentPort& messageContentPort();
        [[nodiscard]] MessageListSessionFactoryPort& messageListSessionFactory();
        [[nodiscard]] MailApplicationEventsPort& mailApplicationEvents();
        [[nodiscard]] CommandDispatcher& commandDispatcher();
        [[nodiscard]] UndoCommandPort& undoCommandPort();
        [[nodiscard]] DeferredSendService& deferredSendService();
        [[nodiscard]] ContactCommandPort& contactCommandPort();
        [[nodiscard]] AccountRuntimeManager& accountRuntimeManager();
        [[nodiscard]] MailQueryApplicationService& mailQueryApplicationService();
        [[nodiscard]] MailMutationApplicationService& mailMutationApplicationService();
        [[nodiscard]] MailNotificationService& mailNotificationService();
        [[nodiscard]] CalendarApplicationService& calendarApplicationService();
        [[nodiscard]] SieveApplicationService& sieveApplicationService();
        [[nodiscard]] MessageNavigationPort& messageNavigationPort();
        [[nodiscard]] ApplicationErrorCoordinator& errorCoordinator();
        [[nodiscard]] CalendarNotificationService& calendarNotificationService();
        [[nodiscard]] CalendarInvitationService& calendarInvitationService();
        [[nodiscard]] WorkScheduler& workScheduler();
        [[nodiscard]] ThreadMaterializationCoordinator& threadMaterializationCoordinator();
        [[nodiscard]] LocalMaintenanceService& localMaintenanceService();
        [[nodiscard]] DeveloperDiagnosticsPort& developerDiagnosticsPort();
        [[nodiscard]] DeveloperMaintenancePort& developerMaintenancePort();
        [[nodiscard]] FullMailSyncService& fullMailSyncService();
        [[nodiscard]] MailIndexService& mailIndexService();
        [[nodiscard]] javelin::app::undo::UndoManager& undoManager();
        [[nodiscard]] javelin::jmap::auth::AccountOnboardingService& onboardingService();
        void setAccessTokenProvider(javelin::jmap::auth::AccessTokenProvider provider);
        void
        setAuthenticationRefreshHandler(javelin::jmap::auth::AccessTokenRefreshHandler handler);

      private:
        javelin::jmap::cache::DatabaseConnection m_databaseConnection;
        QString m_databasePath;
        QUuid m_cacheInstanceId;
        std::unique_ptr<CacheAccessBarrier> m_cacheAccessBarrier;
        std::unique_ptr<MailboxMaintenanceRegistry> m_mailboxMaintenanceRegistry;
        std::unique_ptr<javelin::app::undo::HistoryRepository> m_historyRepository;
        std::unique_ptr<javelin::app::undo::UndoManager> m_undoManager;
        std::unique_ptr<javelin::app::undo::MailTransferHistoryCoordinator>
            m_mailTransferHistoryCoordinator;
        std::unique_ptr<DeferredSendRepository> m_deferredSendRepository;
        std::unique_ptr<QNetworkAccessManager> m_networkAccessManager;
        std::unique_ptr<QNetworkAccessManager> m_stateChangeNetworkAccessManager;
        std::unique_ptr<javelin::jmap::auth::AccountOnboardingService> m_onboardingService;
        std::unique_ptr<javelin::jmap::api::WebSocketFailureCooldowns> m_webSocketFailureCooldowns;
        std::unique_ptr<javelin::jmap::api::QtNetworkTransport> m_networkTransport;
        std::unique_ptr<javelin::jmap::api::RefreshingTransport> m_transport;
        std::unique_ptr<javelin::jmap::api::HttpJmapMethodTransport> m_httpMethodTransport;
        std::unique_ptr<javelin::jmap::api::PreferredJmapMethodTransport>
            m_preferredMethodTransport;
        std::unique_ptr<javelin::jmap::api::RefreshingJmapMethodTransport> m_methodTransport;
        std::unique_ptr<javelin::jmap::SessionRefreshClient> m_sessionRefreshClient;
        std::unique_ptr<javelin::jmap::AccountBootstrapClient> m_accountBootstrapClient;
        std::unique_ptr<javelin::jmap::MailQueryClient> m_mailQueryClient;
        std::unique_ptr<javelin::jmap::MailQueryMaterializer> m_mailQueryMaterializer;
        std::unique_ptr<javelin::jmap::MessageContentClient> m_messageContentClient;
        std::unique_ptr<javelin::jmap::EmailMutationEngine> m_emailMutationEngine;
        std::unique_ptr<javelin::jmap::MailboxMutationEngine> m_mailboxMutationEngine;
        std::unique_ptr<javelin::jmap::cache::AccountRepository> m_accountRepository;
        std::unique_ptr<AccountCommandService> m_accountCommandService;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<javelin::jmap::contacts::ContactProtocolClient> m_contactProtocolClient;
        std::unique_ptr<javelin::jmap::contacts::ContactSyncEngine> m_contactSyncEngine;
        std::unique_ptr<javelin::jmap::contacts::ContactMutationEngine> m_contactMutationEngine;
        std::unique_ptr<javelin::jmap::contacts::ContactMediaService> m_contactMediaService;
        std::unique_ptr<javelin::jmap::calendar::CalendarCacheReader> m_calendarReader;
        std::unique_ptr<javelin::jmap::calendar::CalendarProtocolClient> m_calendarProtocolClient;
        std::unique_ptr<javelin::jmap::calendar::CalendarSyncEngine> m_calendarSyncEngine;
        std::unique_ptr<javelin::jmap::calendar::CalendarMutationEngine> m_calendarMutationEngine;
        std::unique_ptr<CalendarCommandService> m_calendarCommandService;
        std::unique_ptr<javelin::jmap::sieve::SieveProtocolClient> m_sieveProtocolClient;
        std::unique_ptr<javelin::jmap::sieve::SieveMutationEngine> m_sieveMutationEngine;
        std::unique_ptr<javelin::jmap::identity::IdentityService> m_identityService;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::MailboxReadRepository> m_mailboxRepository;
        std::unique_ptr<javelin::jmap::cache::MailTagReadRepository> m_mailTagRepository;
        std::unique_ptr<javelin::jmap::cache::MailboxStatisticsReadRepository>
            m_mailboxStatisticsRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::MailboxMessageReadRepository>
            m_mailboxMessageRepository;
        std::unique_ptr<javelin::jmap::cache::MailboxFilterReadRepository>
            m_mailboxFilterRepository;
        std::unique_ptr<javelin::jmap::cache::SubmissionRepository> m_submissionRepository;
        std::unique_ptr<javelin::jmap::submission::ComposeService> m_jmapComposeService;
        std::unique_ptr<DeferredSendSubmitter> m_deferredSendSubmitter;
        std::unique_ptr<ComposeService> m_composeService;
        std::unique_ptr<ComposeCommandService> m_composeCommandService;
        std::unique_ptr<DeferredSendService> m_deferredSendService;
        std::unique_ptr<ApplicationErrorCoordinator> m_errorCoordinator;
        std::unique_ptr<AccountRuntimeManager> m_accountRuntimeManager;
        std::unique_ptr<MailQueryApplicationService> m_mailQueryApplicationService;
        std::unique_ptr<MailMutationApplicationService> m_mailMutationApplicationService;
        std::unique_ptr<MessageContentApplicationService> m_messageContentApplicationService;
        std::unique_ptr<MailNotificationService> m_mailNotificationService;
        std::unique_ptr<ContactApplicationService> m_contactApplicationService;
        std::unique_ptr<CalendarApplicationService> m_calendarApplicationService;
        std::unique_ptr<SieveApplicationService> m_sieveApplicationService;
        std::unique_ptr<MailTransferWorkService> m_mailTransferWorkService;
        std::unique_ptr<MailExportService> m_mailExportService;
        std::unique_ptr<MailTransferCommandService> m_mailTransferCommandService;
        std::unique_ptr<MailCommandService> m_mailCommandService;
        std::unique_ptr<SieveCommandService> m_sieveCommandService;
        std::unique_ptr<IdentityCommandService> m_identityCommandService;
        std::unique_ptr<AccountRefreshCommandService> m_accountRefreshCommandService;
        std::unique_ptr<MessageContentCommandService> m_messageContentCommandService;
        std::unique_ptr<MailApplicationEventsService> m_mailApplicationEventsService;
        std::unique_ptr<MessageListSessionFactoryService> m_messageListSessionFactoryService;
        std::unique_ptr<CommandDispatcher> m_commandDispatcher;
        std::unique_ptr<UndoCommandService> m_undoCommandService;
        std::unique_ptr<javelin::app::undo::MailHistoryExecutor> m_mailHistoryExecutor;
        std::unique_ptr<javelin::app::undo::MailTransferHistoryService>
            m_mailTransferHistoryService;
        std::unique_ptr<javelin::app::undo::MailTransferHistoryExecutor>
            m_mailTransferHistoryExecutor;
        std::unique_ptr<javelin::app::undo::DraftHistoryExecutor> m_draftHistoryExecutor;
        std::unique_ptr<javelin::app::undo::SieveHistoryExecutor> m_sieveHistoryExecutor;
        std::unique_ptr<javelin::app::undo::CalendarHistoryExecutor> m_calendarHistoryExecutor;
        std::unique_ptr<javelin::app::undo::CalendarPreferenceExecutor>
            m_calendarPreferenceExecutor;
        std::unique_ptr<javelin::app::undo::ContactHistoryExecutor> m_contactHistoryExecutor;
        std::unique_ptr<javelin::app::undo::AddressBookHistoryExecutor>
            m_addressBookHistoryExecutor;
        std::unique_ptr<ContactCommandService> m_contactCommandService;
        std::unique_ptr<MessageNavigationCoordinator> m_messageNavigationCoordinator;
        std::unique_ptr<CalendarNotificationService> m_calendarNotificationService;
        std::unique_ptr<CalendarInvitationService> m_calendarInvitationService;
        std::unique_ptr<WorkScheduler> m_workScheduler;
        std::unique_ptr<ThreadMaterializationCoordinator> m_threadMaterializationCoordinator;
        std::unique_ptr<ThreadMembershipMaterializationWorker>
            m_threadMembershipMaterializationWorker;
        std::unique_ptr<MailIndexService> m_mailIndexService;
        std::unique_ptr<FullMailSyncService> m_fullMailSyncService;
        std::unique_ptr<LocalMaintenanceService> m_localMaintenanceService;
        std::unique_ptr<DeveloperDiagnosticsService> m_developerDiagnosticsService;
        std::unique_ptr<DeveloperMaintenanceService> m_developerMaintenanceService;
    };

} // namespace javelin::app
