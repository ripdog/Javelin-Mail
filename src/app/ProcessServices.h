#pragma once

#include "jmap/cache/Database.h"

#include <memory>

class QNetworkAccessManager;

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::jmap::contacts
{
    class ContactService;
    class ContactIdentityLookup;
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
    class DeferredSendService;
    class AccountCommandPort;
    class AccountCommandService;
    class ContactCommandPort;
    class ContactCommandService;
    class CalendarNotificationService;
    class CalendarCommandPort;
    class CalendarCommandService;
    class MessageNavigationCoordinator;
    class InlineMessageSchemeHandler;
    class MailApplicationService;
    class TranslationService;
    class TranslationPort;
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
    class QtNetworkTransport;
    class WebSocketFailureCooldowns;
} // namespace javelin::jmap::api

namespace javelin::jmap::cache
{
    class AccountReader;
    class AccountReadRepository;
    class AccountRepository;
    class MailboxReader;
    class MailboxReadRepository;
    class ContactRepository;
    class ContactReader;
    class IdentityRepository;
    class IdentityReader;
    class MessageViewService;
    class MessageViewReader;
    class QueryService;
    class QueryReader;
    class SubmissionRepository;
    class TranslationCacheRepository;
} // namespace javelin::jmap::cache

namespace javelin::jmap::submission
{
    class ComposeService;
}
namespace javelin::jmap::calendar
{
    class CalendarService;
    class CalendarReader;
    class CalendarReadService;
} // namespace javelin::jmap::calendar
namespace javelin::jmap::sieve
{
    class SieveService;
}

namespace javelin::app
{

    class ProcessServices
    {
      public:
        explicit ProcessServices(bool installInlineMessageSchemeHandler = true);
        ~ProcessServices();

        ProcessServices(const ProcessServices&) = delete;
        ProcessServices& operator=(const ProcessServices&) = delete;
        ProcessServices(ProcessServices&&) = delete;
        ProcessServices& operator=(ProcessServices&&) = delete;

        [[nodiscard]] javelin::jmap::cache::AccountRepository& accountRepository();
        [[nodiscard]] AccountCommandPort& accountCommandPort();
        [[nodiscard]] javelin::jmap::cache::AccountReader& accountReader();
        [[nodiscard]] javelin::jmap::cache::MailboxReader& mailboxReader();
        [[nodiscard]] javelin::jmap::cache::DatabaseConnection& databaseConnection();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> suspendGuiCacheAccess();
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> resumeGuiCacheAccess();
        [[nodiscard]] javelin::jmap::cache::ContactRepository& contactRepository();
        [[nodiscard]] javelin::jmap::cache::ContactReader& contactReader();
        [[nodiscard]] javelin::jmap::contacts::ContactService& contactService();
        [[nodiscard]] javelin::jmap::calendar::CalendarService& calendarService();
        [[nodiscard]] javelin::jmap::calendar::CalendarReader& calendarReader();
        [[nodiscard]] CalendarCommandPort& calendarCommandPort();
        [[nodiscard]] javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup();
        [[nodiscard]] javelin::jmap::cache::IdentityRepository& identityRepository();
        [[nodiscard]] javelin::jmap::cache::IdentityReader& identityReader();
        [[nodiscard]] javelin::jmap::cache::MessageViewService& messageViewService();
        [[nodiscard]] javelin::jmap::cache::MessageViewReader& messageViewReader();
        [[nodiscard]] javelin::jmap::cache::QueryReader& queryReader();
        [[nodiscard]] javelin::jmap::cache::QueryService& queryService();
        [[nodiscard]] TranslationPort& translationService();
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
        [[nodiscard]] MessageNavigationCoordinator& messageNavigationCoordinator();
        [[nodiscard]] ApplicationErrorCoordinator& errorCoordinator();
        [[nodiscard]] CalendarNotificationService& calendarNotificationService();
        [[nodiscard]] WorkScheduler& workScheduler();
        [[nodiscard]] LocalMaintenanceService& localMaintenanceService();
        [[nodiscard]] FullMailSyncService& fullMailSyncService();
        [[nodiscard]] MailIndexService& mailIndexService();
        [[nodiscard]] javelin::app::undo::UndoManager& undoManager();

      private:
        std::unique_ptr<javelin::jmap::JmapCore> m_jmapCore;
        javelin::jmap::cache::DatabaseConnection m_databaseConnection;
        javelin::jmap::cache::ReadOnlyDatabaseConnection m_guiReadDatabaseConnection;
        QString m_guiDatabasePath;
        std::unique_ptr<CacheAccessBarrier> m_cacheAccessBarrier;
        std::unique_ptr<javelin::app::undo::HistoryRepository> m_historyRepository;
        std::unique_ptr<javelin::app::undo::UndoManager> m_undoManager;
        std::unique_ptr<DeferredSendRepository> m_deferredSendRepository;
        std::unique_ptr<QNetworkAccessManager> m_networkAccessManager;
        std::unique_ptr<QNetworkAccessManager> m_stateChangeNetworkAccessManager;
        std::unique_ptr<javelin::jmap::api::WebSocketFailureCooldowns> m_webSocketFailureCooldowns;
        std::unique_ptr<javelin::jmap::api::QtNetworkTransport> m_transport;
        std::unique_ptr<javelin::jmap::api::HttpJmapMethodTransport> m_httpMethodTransport;
        std::unique_ptr<javelin::jmap::api::PreferredJmapMethodTransport> m_methodTransport;
        std::unique_ptr<InlineMessageSchemeHandler> m_inlineMessageSchemeHandler;
        std::unique_ptr<javelin::jmap::cache::AccountRepository> m_accountRepository;
        std::unique_ptr<AccountCommandService> m_accountCommandService;
        std::unique_ptr<javelin::jmap::cache::AccountReadRepository> m_accountReadRepository;
        std::unique_ptr<javelin::jmap::cache::MailboxReadRepository> m_mailboxReadRepository;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_guiContactRepository;
        std::unique_ptr<javelin::jmap::contacts::ContactService> m_contactService;
        std::unique_ptr<javelin::jmap::calendar::CalendarService> m_calendarService;
        std::unique_ptr<CalendarCommandService> m_calendarCommandService;
        std::unique_ptr<javelin::jmap::calendar::CalendarReadService> m_calendarReadService;
        std::unique_ptr<javelin::jmap::sieve::SieveService> m_sieveService;
        std::unique_ptr<javelin::jmap::contacts::ContactIdentityLookup> m_contactIdentityLookup;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_guiIdentityRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_guiMessageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_guiQueryService;
        std::unique_ptr<javelin::jmap::cache::TranslationCacheRepository>
            m_translationCacheRepository;
        std::unique_ptr<TranslationService> m_translationService;
        std::unique_ptr<javelin::jmap::cache::SubmissionRepository> m_submissionRepository;
        std::unique_ptr<javelin::jmap::submission::ComposeService> m_jmapComposeService;
        std::unique_ptr<ComposeService> m_composeService;
        std::unique_ptr<ComposeCommandService> m_composeCommandService;
        std::unique_ptr<DeferredSendService> m_deferredSendService;
        std::unique_ptr<ApplicationErrorCoordinator> m_errorCoordinator;
        std::unique_ptr<MailApplicationService> m_mailService;
        std::unique_ptr<MailCommandService> m_mailCommandService;
        std::unique_ptr<SieveCommandService> m_sieveCommandService;
        std::unique_ptr<AccountRefreshCommandService> m_accountRefreshCommandService;
        std::unique_ptr<MessageContentCommandService> m_messageContentCommandService;
        std::unique_ptr<MessageListSessionFactoryService> m_messageListSessionFactoryService;
        std::unique_ptr<MailApplicationEventsService> m_mailApplicationEventsService;
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
    };

} // namespace javelin::app
