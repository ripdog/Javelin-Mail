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
    class FullMailSyncService;
    class MailIndexService;
    class LocalMaintenanceService;
    class ApplicationErrorCoordinator;
    class ComposeService;
    class DeferredSendRepository;
    class DeferredSendService;
    class ContactCommandPort;
    class ContactCommandService;
    class CalendarNotificationService;
    class MessageNavigationCoordinator;
    class InlineMessageSchemeHandler;
    class MailApplicationService;
    class TranslationService;
} // namespace javelin::app

namespace javelin::app::undo
{
    class HistoryRepository;
    class DraftHistoryExecutor;
    class CalendarHistoryExecutor;
    class CalendarPreferenceExecutor;
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
    class AccountRepository;
    class ContactRepository;
    class IdentityRepository;
    class MessageViewService;
    class QueryService;
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
}
namespace javelin::jmap::sieve
{
    class SieveService;
}

namespace javelin::app
{

    class ProcessServices
    {
      public:
        ProcessServices();
        ~ProcessServices();

        ProcessServices(const ProcessServices&) = delete;
        ProcessServices& operator=(const ProcessServices&) = delete;
        ProcessServices(ProcessServices&&) = delete;
        ProcessServices& operator=(ProcessServices&&) = delete;

        [[nodiscard]] javelin::jmap::cache::AccountRepository& accountRepository();
        [[nodiscard]] javelin::jmap::cache::ContactRepository& contactRepository();
        [[nodiscard]] javelin::jmap::contacts::ContactService& contactService();
        [[nodiscard]] javelin::jmap::calendar::CalendarService& calendarService();
        [[nodiscard]] javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup();
        [[nodiscard]] javelin::jmap::cache::IdentityRepository& identityRepository();
        [[nodiscard]] javelin::jmap::cache::MessageViewService& messageViewService();
        [[nodiscard]] javelin::jmap::cache::QueryService& queryService();
        [[nodiscard]] TranslationService& translationService();
        [[nodiscard]] ComposeService& composeService();
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
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<javelin::jmap::contacts::ContactService> m_contactService;
        std::unique_ptr<javelin::jmap::calendar::CalendarService> m_calendarService;
        std::unique_ptr<javelin::jmap::sieve::SieveService> m_sieveService;
        std::unique_ptr<javelin::jmap::contacts::ContactIdentityLookup> m_contactIdentityLookup;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
        std::unique_ptr<javelin::jmap::cache::TranslationCacheRepository>
            m_translationCacheRepository;
        std::unique_ptr<TranslationService> m_translationService;
        std::unique_ptr<javelin::jmap::cache::SubmissionRepository> m_submissionRepository;
        std::unique_ptr<javelin::jmap::submission::ComposeService> m_jmapComposeService;
        std::unique_ptr<ComposeService> m_composeService;
        std::unique_ptr<DeferredSendService> m_deferredSendService;
        std::unique_ptr<ApplicationErrorCoordinator> m_errorCoordinator;
        std::unique_ptr<MailApplicationService> m_mailService;
        std::unique_ptr<javelin::app::undo::MailHistoryExecutor> m_mailHistoryExecutor;
        std::unique_ptr<javelin::app::undo::DraftHistoryExecutor> m_draftHistoryExecutor;
        std::unique_ptr<javelin::app::undo::SieveHistoryExecutor> m_sieveHistoryExecutor;
        std::unique_ptr<javelin::app::undo::CalendarHistoryExecutor> m_calendarHistoryExecutor;
        std::unique_ptr<javelin::app::undo::CalendarPreferenceExecutor>
            m_calendarPreferenceExecutor;
        std::unique_ptr<ContactCommandService> m_contactCommandService;
        std::unique_ptr<MessageNavigationCoordinator> m_messageNavigationCoordinator;
        std::unique_ptr<CalendarNotificationService> m_calendarNotificationService;
        std::unique_ptr<WorkScheduler> m_workScheduler;
        std::unique_ptr<FullMailSyncService> m_fullMailSyncService;
        std::unique_ptr<MailIndexService> m_mailIndexService;
        std::unique_ptr<LocalMaintenanceService> m_localMaintenanceService;
    };

} // namespace javelin::app
