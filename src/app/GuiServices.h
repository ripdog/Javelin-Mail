#pragma once

#include "app/CacheAccessBarrier.h"
#include "jmap/cache/Database.h"
#include "protocol/ProcessBoundary.h"

#include <QMetaObject>

#include <memory>

class QNetworkAccessManager;

namespace javelin::gui::settings
{
    class GuiSettings;
}
namespace javelin::gui::translation
{
    class BergamotTranslationBackend;
    class GoogleTranslationBackend;
    class TranslationCache;
    class TranslationModelManifest;
    class TranslationModelStore;
    class TranslationService;
    class TranslationSettingsStore;
} // namespace javelin::gui::translation

namespace javelin::jmap::cache
{
    class AccountReader;
    class AccountReadRepository;
    class MailboxReader;
    class MailboxReadRepository;
    class ContactReader;
    class ContactRepository;
    class IdentityReader;
    class IdentityRepository;
    class MessageViewReader;
    class MessageViewService;
    class QueryReader;
    class QueryService;
} // namespace javelin::jmap::cache

namespace javelin::jmap::calendar
{
    class CalendarReader;
} // namespace javelin::jmap::calendar

namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
} // namespace javelin::jmap::contacts

namespace javelin::app
{
    class AccountCommandPort;
    class AccountRefreshPort;
    class CalendarCommandPort;
    class ComposeCommandPort;
    class ContactCommandPort;
    class DeveloperDiagnosticsPort;
    class DeveloperMaintenancePort;
    class GuiDaemonSession;
    class GuiMailApplicationEvents;
    class InlineMessageSchemeHandler;
    class MailApplicationEventsPort;
    class MailCommandPort;
    class MessageContentPort;
    class MessageListSessionFactoryPort;
    class MessageListSessionFactoryService;
    class MessageNavigationCoordinator;
    class MessageNavigationPort;
    class RemoteAccountCommandPort;
    class RemoteAccountRefreshPort;
    class RemoteActionClient;
    class RemoteCalendarCommandPort;
    class RemoteCalendarReader;
    class RemoteComposeCommandPort;
    class RemoteContactCommandPort;
    class RemoteDeveloperDiagnosticsPort;
    class RemoteDeveloperMaintenancePort;
    class RemoteMailCommandPort;
    class RemoteMessageContentPort;
    class RemoteMessageListMaterializationPort;
    class RemoteOnboardingPort;
    class RemoteSieveCommandPort;
    class RemoteUndoCommandPort;
    class RemoteWorkTaskPort;
    class SieveCommandPort;
    class UndoCommandPort;
    class WorkTaskPort;
    class OnboardingPort;

    class GuiServices final
    {
      public:
        explicit GuiServices(GuiDaemonSession& session,
                             bool installInlineMessageSchemeHandler = true);
        ~GuiServices();

        GuiServices(const GuiServices&) = delete;
        GuiServices& operator=(const GuiServices&) = delete;
        GuiServices(GuiServices&&) = delete;
        GuiServices& operator=(GuiServices&&) = delete;

        [[nodiscard]] javelin::jmap::cache::AccountReader& accountReader();
        [[nodiscard]] javelin::jmap::cache::MailboxReader& mailboxReader();
        [[nodiscard]] javelin::jmap::cache::ContactReader& contactReader();
        [[nodiscard]] javelin::jmap::calendar::CalendarReader& calendarReader();
        [[nodiscard]] javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup();
        [[nodiscard]] javelin::jmap::cache::IdentityReader& identityReader();
        [[nodiscard]] javelin::jmap::cache::MessageViewReader& messageViewReader();
        [[nodiscard]] javelin::jmap::cache::QueryReader& queryReader();

        [[nodiscard]] AccountCommandPort& accountCommandPort();
        [[nodiscard]] CalendarCommandPort& calendarCommandPort();
        [[nodiscard]] ComposeCommandPort& composeCommandPort();
        [[nodiscard]] ContactCommandPort& contactCommandPort();
        [[nodiscard]] MailCommandPort& mailCommandPort();
        [[nodiscard]] SieveCommandPort& sieveCommandPort();
        [[nodiscard]] AccountRefreshPort& accountRefreshPort();
        [[nodiscard]] MessageContentPort& messageContentPort();
        [[nodiscard]] MessageListSessionFactoryPort& messageListSessionFactory();
        [[nodiscard]] MailApplicationEventsPort& mailEvents();
        [[nodiscard]] MessageNavigationPort& messageNavigationPort();
        [[nodiscard]] UndoCommandPort& undoCommandPort();
        [[nodiscard]] DeveloperDiagnosticsPort& developerDiagnosticsPort();
        [[nodiscard]] DeveloperMaintenancePort& developerMaintenancePort();
        [[nodiscard]] javelin::gui::translation::TranslationService& translationService();
        [[nodiscard]] WorkTaskPort& workTaskPort();
        [[nodiscard]] OnboardingPort& onboardingPort();
        [[nodiscard]] javelin::gui::settings::GuiSettings& settings();

      private:
        [[nodiscard]] std::optional<javelin::jmap::cache::DatabaseError> openReadConnection();
        void notifyCacheReaders(const javelin::protocol::CacheInvalidation& invalidation);

        GuiDaemonSession& m_session;
        javelin::jmap::cache::ReadOnlyDatabaseConnection m_databaseConnection;
        CacheAccessBarrier::ParticipantId m_cacheParticipant = 0;
        QMetaObject::Connection m_cacheInvalidationConnection;

        std::unique_ptr<javelin::jmap::cache::AccountReadRepository> m_accountRepository;
        std::unique_ptr<javelin::jmap::cache::MailboxReadRepository> m_mailboxRepository;
        std::unique_ptr<javelin::jmap::cache::ContactRepository> m_contactRepository;
        std::unique_ptr<RemoteCalendarReader> m_calendarReader;
        std::unique_ptr<javelin::jmap::contacts::ContactIdentityLookup> m_contactIdentityLookup;
        std::unique_ptr<javelin::jmap::cache::IdentityRepository> m_identityRepository;
        std::unique_ptr<javelin::jmap::cache::MessageViewService> m_messageViewService;
        std::unique_ptr<javelin::jmap::cache::QueryService> m_queryService;
        std::unique_ptr<InlineMessageSchemeHandler> m_inlineMessageSchemeHandler;

        std::unique_ptr<javelin::gui::settings::GuiSettings> m_settings;
        std::unique_ptr<QNetworkAccessManager> m_networkAccessManager;
        std::unique_ptr<javelin::gui::translation::TranslationSettingsStore>
            m_translationSettingsStore;
        std::unique_ptr<javelin::gui::translation::TranslationCache> m_translationCache;
        std::unique_ptr<javelin::gui::translation::GoogleTranslationBackend>
            m_googleTranslationBackend;
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        std::unique_ptr<javelin::gui::translation::TranslationModelManifest>
            m_translationModelManifest;
        std::unique_ptr<javelin::gui::translation::TranslationModelStore> m_translationModelStore;
        std::unique_ptr<javelin::gui::translation::BergamotTranslationBackend>
            m_bergamotTranslationBackend;
#endif
        std::unique_ptr<javelin::gui::translation::TranslationService> m_translationService;
        std::unique_ptr<RemoteActionClient> m_remoteClient;
        std::unique_ptr<RemoteAccountCommandPort> m_accountCommands;
        std::unique_ptr<RemoteCalendarCommandPort> m_calendarCommands;
        std::unique_ptr<RemoteComposeCommandPort> m_composeCommands;
        std::unique_ptr<RemoteContactCommandPort> m_contactCommands;
        std::unique_ptr<RemoteDeveloperDiagnosticsPort> m_developerDiagnostics;
        std::unique_ptr<RemoteDeveloperMaintenancePort> m_developerMaintenance;
        std::unique_ptr<RemoteMailCommandPort> m_mailCommands;
        std::unique_ptr<RemoteSieveCommandPort> m_sieveCommands;
        std::unique_ptr<RemoteAccountRefreshPort> m_accountRefresh;
        std::unique_ptr<RemoteMessageContentPort> m_messageContent;
        std::unique_ptr<RemoteMessageListMaterializationPort> m_materialization;
        std::unique_ptr<GuiMailApplicationEvents> m_mailEvents;
        std::unique_ptr<MessageListSessionFactoryService> m_messageListSessions;
        std::unique_ptr<MessageNavigationCoordinator> m_messageNavigation;
        std::unique_ptr<RemoteUndoCommandPort> m_undoCommands;
        std::unique_ptr<RemoteWorkTaskPort> m_workTasks;
        std::unique_ptr<RemoteOnboardingPort> m_onboarding;
    };
} // namespace javelin::app
