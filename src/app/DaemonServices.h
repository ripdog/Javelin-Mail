#pragma once

#include "app/CacheLocationProvider.h"
#include "jmap/auth/Auth.h"
#include "jmap/cache/Database.h"
#include "protocol/ProcessBoundary.h"

#include <memory>

class QNetworkAccessManager;

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::jmap::auth
{
    class AccountOnboardingService;
}

namespace javelin::jmap::contacts
{
    class ContactService;
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
    class SieveCommandPort;
    class SieveCommandService;
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
    class CalendarCommandPort;
    class CalendarCommandService;
    class MessageNavigationPort;
    class MessageNavigationCoordinator;
    class MailApplicationService;
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
    class MessageViewService;
    class QueryService;
    class SubmissionRepository;
} // namespace javelin::jmap::cache

namespace javelin::jmap::submission
{
    class ComposeService;
}
namespace javelin::jmap::calendar
{
    class CalendarService;
} // namespace javelin::jmap::calendar
namespace javelin::jmap::sieve
{
    class SieveService;
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
        [[nodiscard]] javelin::jmap::contacts::ContactService& contactService();
        [[nodiscard]] javelin::jmap::calendar::CalendarService& calendarService();
        [[nodiscard]] CalendarCommandPort& calendarCommandPort();
        [[nodiscard]] javelin::jmap::cache::IdentityRepository& identityRepository();
        [[nodiscard]] javelin::jmap::cache::MessageViewService& messageViewService();
        [[nodiscard]] javelin::jmap::cache::QueryService& queryService();
        [[nodiscard]] ComposeService& composeService();
        [[nodiscard]] ComposeCommandPort& composeCommandPort();
        [[nodiscard]] MailCommandPort& mailCommandPort();
        [[nodiscard]] SieveCommandPort& sieveCommandPort();
        [[nodiscard]] AccountRefreshPort& accountRefreshPort();
        [[nodiscard]] MessageContentPort& messageContentPort();
        [[nodiscard]] MessageListSessionFactoryPort& messageListSessionFactory();
        [[nodiscard]] MailApplicationEventsPort& mailApplicationEvents();
        [[nodiscard]] CommandDispatcher& commandDispatcher();
        [[nodiscard]] UndoCommandPort& undoCommandPort();
        [[nodiscard]] DeferredSendService& deferredSendService();
        [[nodiscard]] ContactCommandPort& contactCommandPort();
        [[nodiscard]] MailApplicationService& mailService();
        [[nodiscard]] MessageNavigationPort& messageNavigationPort();
        [[nodiscard]] ApplicationErrorCoordinator& errorCoordinator();
        [[nodiscard]] CalendarNotificationService& calendarNotificationService();
        [[nodiscard]] WorkScheduler& workScheduler();
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
        std::unique_ptr<javelin::jmap::JmapCore> m_jmapCore;
        javelin::jmap::cache::DatabaseConnection m_databaseConnection;
        QString m_databasePath;
        QUuid m_cacheInstanceId;
        std::unique_ptr<CacheAccessBarrier> m_cacheAccessBarrier;
        std::unique_ptr<MailboxMaintenanceRegistry> m_mailboxMaintenanceRegistry;
        std::unique_ptr<javelin::app::undo::HistoryRepository> m_historyRepository;
        std::unique_ptr<javelin::app::undo::UndoManager> m_undoManager;
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
        std::unique_ptr<javelin::jmap::cache::AccountRepository> m_accountRepository;
        std::unique_ptr<AccountCommandService> m_accountCommandService;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<javelin::jmap::contacts::ContactService> m_contactService;
        std::unique_ptr<javelin::jmap::calendar::CalendarService> m_calendarService;
        std::unique_ptr<CalendarCommandService> m_calendarCommandService;
        std::unique_ptr<javelin::jmap::sieve::SieveService> m_sieveService;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
        std::unique_ptr<javelin::jmap::cache::SubmissionRepository> m_submissionRepository;
        std::unique_ptr<javelin::jmap::submission::ComposeService> m_jmapComposeService;
        std::unique_ptr<DeferredSendSubmitter> m_deferredSendSubmitter;
        std::unique_ptr<ComposeService> m_composeService;
        std::unique_ptr<ComposeCommandService> m_composeCommandService;
        std::unique_ptr<DeferredSendService> m_deferredSendService;
        std::unique_ptr<ApplicationErrorCoordinator> m_errorCoordinator;
        std::unique_ptr<MailApplicationService> m_mailService;
        std::unique_ptr<MailCommandService> m_mailCommandService;
        std::unique_ptr<SieveCommandService> m_sieveCommandService;
        std::unique_ptr<AccountRefreshCommandService> m_accountRefreshCommandService;
        std::unique_ptr<MessageContentCommandService> m_messageContentCommandService;
        std::unique_ptr<MailApplicationEventsService> m_mailApplicationEventsService;
        std::unique_ptr<MessageListSessionFactoryService> m_messageListSessionFactoryService;
        std::unique_ptr<CommandDispatcher> m_commandDispatcher;
        std::unique_ptr<UndoCommandService> m_undoCommandService;
        std::unique_ptr<javelin::app::undo::MailHistoryExecutor> m_mailHistoryExecutor;
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
        std::unique_ptr<WorkScheduler> m_workScheduler;
        std::unique_ptr<FullMailSyncService> m_fullMailSyncService;
        std::unique_ptr<MailIndexService> m_mailIndexService;
        std::unique_ptr<LocalMaintenanceService> m_localMaintenanceService;
        std::unique_ptr<DeveloperDiagnosticsService> m_developerDiagnosticsService;
        std::unique_ptr<DeveloperMaintenanceService> m_developerMaintenanceService;
    };

} // namespace javelin::app
