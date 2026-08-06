#include "app/GuiServices.h"

#include "app/AddressSuggestionStore.h"
#include "app/GuiDaemonSession.h"
#include "app/GuiMailApplicationEvents.h"
#include "app/InlineMessageSchemeHandler.h"
#include "app/MessageListSessionFactoryService.h"
#include "app/MessageNavigationCoordinator.h"
#include "app/RemoteActionClient.h"
#include "app/RemoteApplicationPorts.h"
#include "app/RemoteOnboardingPort.h"

#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/calendar/CalendarReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/render/InlineMessageUrl.h"

#include "gui/settings/GuiSettings.h"
#include "gui/translation/GoogleTranslationBackend.h"
#include "gui/translation/TranslationCache.h"
#include "gui/translation/TranslationService.h"
#include "gui/translation/TranslationSettingsStore.h"
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
#include "gui/translation/BergamotTranslationBackend.h"
#include "gui/translation/TranslationModelManifest.h"
#include "gui/translation/TranslationModelStore.h"
#endif

#include <QDebug>
#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QWebEngineProfile>

#include <algorithm>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] std::string languageModelPath()
        {
            const auto installedPath =
                QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                       QStringLiteral("javelinmail/models/fasttext/lid.176.ftz"),
                                       QStandardPaths::LocateFile);
            if (!installedPath.isEmpty())
            {
                return installedPath.toStdString();
            }
#ifdef JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH
            return JAVELIN_FASTTEXT_LANGUAGE_MODEL_PATH;
#else
            return {};
#endif
        }
    } // namespace

    GuiServices::GuiServices(GuiDaemonSession& session,
                             const bool installInlineMessageSchemeHandler)
        : m_session(session)
    {
        if (const auto error = openReadConnection())
            throw std::runtime_error(error->message.toStdString());

        m_cacheParticipant = m_session.registerCacheParticipant({
            .name = QStringLiteral("GUI cache connections"),
            .suspend = [this]() -> std::optional<javelin::jmap::cache::DatabaseError>
            {
                m_databaseConnection = javelin::jmap::cache::ReadOnlyDatabaseConnection{};
                return std::nullopt;
            },
            .resume = [this] { return openReadConnection(); },
        });

        m_accountRepository =
            std::make_unique<javelin::jmap::cache::AccountReadRepository>(m_databaseConnection);
        m_mailboxRepository =
            std::make_unique<javelin::jmap::cache::MailboxReadRepository>(m_databaseConnection);
        m_contactRepository =
            std::make_unique<javelin::jmap::cache::ContactRepository>(m_databaseConnection);
        m_contactIdentityLookup =
            std::make_unique<javelin::jmap::contacts::ContactIdentityLookup>(*m_contactRepository);
        m_identityRepository =
            std::make_unique<javelin::jmap::cache::IdentityRepository>(m_databaseConnection);
        m_messageViewService =
            std::make_unique<javelin::jmap::cache::MessageViewService>(m_databaseConnection);
        m_queryService = std::make_unique<javelin::jmap::cache::QueryService>(m_databaseConnection);

        AddressSuggestionStore::instance().initialize(m_session.databasePath());
        m_cacheInvalidationConnection =
            QObject::connect(&m_session, &GuiDaemonSession::cacheInvalidated, &m_session,
                             [this](const javelin::protocol::CacheInvalidation& invalidation)
                             { notifyCacheReaders(invalidation); });

        if (installInlineMessageSchemeHandler)
        {
            m_inlineMessageSchemeHandler =
                std::make_unique<InlineMessageSchemeHandler>(m_databaseConnection);
            QWebEngineProfile::defaultProfile()->installUrlSchemeHandler(
                javelin::jmap::render::inlineMessageUrlScheme().toUtf8(),
                m_inlineMessageSchemeHandler.get());
        }

        m_settings = std::make_unique<javelin::gui::settings::GuiSettings>(m_session);
        m_networkAccessManager = std::make_unique<QNetworkAccessManager>();
        m_translationSettingsStore =
            std::make_unique<javelin::gui::translation::TranslationSettingsStore>();
        m_translationCache = std::make_unique<javelin::gui::translation::TranslationCache>();
        m_googleTranslationBackend =
            std::make_unique<javelin::gui::translation::GoogleTranslationBackend>(
                *m_networkAccessManager);
        javelin::gui::translation::TranslationBackend* localBackend = nullptr;
        javelin::gui::translation::TranslationModelManifest* localManifest = nullptr;
        javelin::gui::translation::TranslationModelStore* localModelStore = nullptr;
#if JAVELIN_ENABLE_BERGAMOT_TRANSLATION
        javelin::gui::translation::TranslationError manifestError;
        m_translationModelManifest =
            javelin::gui::translation::TranslationModelManifest::fromResource(manifestError);
        if (m_translationModelManifest != nullptr)
        {
            m_translationModelStore =
                std::make_unique<javelin::gui::translation::TranslationModelStore>(
                    *m_translationModelManifest, *m_networkAccessManager);
            m_bergamotTranslationBackend =
                std::make_unique<javelin::gui::translation::BergamotTranslationBackend>(
                    *m_translationModelManifest, *m_translationModelStore);
            localBackend = m_bergamotTranslationBackend.get();
            localManifest = m_translationModelManifest.get();
            localModelStore = m_translationModelStore.get();
        }
        else
        {
            qWarning().noquote() << "Local translation disabled:" << manifestError.message;
        }
#endif
        m_translationService = std::make_unique<javelin::gui::translation::TranslationService>(
            *m_translationSettingsStore, *m_translationCache, *m_googleTranslationBackend,
            languageModelPath(), localBackend, localManifest, localModelStore);
        m_remoteClient = std::make_unique<RemoteActionClient>(m_session);
        m_calendarReader = std::make_unique<RemoteCalendarReader>(*m_remoteClient);
        m_accountCommands = std::make_unique<RemoteAccountCommandPort>(*m_remoteClient);
        m_calendarCommands = std::make_unique<RemoteCalendarCommandPort>(*m_remoteClient);
        m_composeCommands = std::make_unique<RemoteComposeCommandPort>(*m_remoteClient);
        m_contactCommands = std::make_unique<RemoteContactCommandPort>(*m_remoteClient);
        m_developerDiagnostics = std::make_unique<RemoteDeveloperDiagnosticsPort>(*m_remoteClient);
        m_developerMaintenance = std::make_unique<RemoteDeveloperMaintenancePort>(*m_remoteClient);
        m_mailCommands = std::make_unique<RemoteMailCommandPort>(*m_remoteClient);
        m_sieveCommands = std::make_unique<RemoteSieveCommandPort>(*m_remoteClient);
        m_identityCommands = std::make_unique<RemoteIdentityCommandPort>(*m_remoteClient);
        m_accountRefresh = std::make_unique<RemoteAccountRefreshPort>(m_session, *m_remoteClient);
        m_messageContent = std::make_unique<RemoteMessageContentPort>(*m_remoteClient);
        m_materialization = std::make_unique<RemoteMessageListMaterializationPort>(*m_remoteClient);
        m_mailEvents = std::make_unique<GuiMailApplicationEvents>(m_session);
        m_messageListSessions =
            std::make_unique<MessageListSessionFactoryService>(*m_materialization, *m_mailEvents);
        m_messageNavigation = std::make_unique<MessageNavigationCoordinator>();
        m_undoCommands = std::make_unique<RemoteUndoCommandPort>(m_session, *m_remoteClient);
        m_workTasks = std::make_unique<RemoteWorkTaskPort>(m_session, *m_remoteClient);
        m_onboarding = std::make_unique<RemoteOnboardingPort>(*m_remoteClient);
    }

    GuiServices::~GuiServices()
    {
        QObject::disconnect(m_cacheInvalidationConnection);
        if (m_inlineMessageSchemeHandler != nullptr)
        {
            QWebEngineProfile::defaultProfile()->removeUrlSchemeHandler(
                m_inlineMessageSchemeHandler.get());
        }
        if (m_cacheParticipant != 0)
            m_session.unregisterCacheParticipant(m_cacheParticipant);
    }

    std::optional<javelin::jmap::cache::DatabaseError> GuiServices::openReadConnection()
    {
        auto opened = javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
                .databasePath = m_session.databasePath(),
            }}.openForCurrentThread("main");
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            return *error;
        m_databaseConnection =
            std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(opened));
        return std::nullopt;
    }

    void GuiServices::notifyCacheReaders(const javelin::protocol::CacheInvalidation& invalidation)
    {
        if (invalidation.affectedKeys.empty())
            return;
        const auto accountId = invalidation.affectedKeys.front();
        if (std::ranges::contains(invalidation.changedDomains,
                                  javelin::protocol::ChangedDomain::Contacts))
        {
            m_contactRepository->notifyChanged(accountId.toStdString());
            AddressSuggestionStore::instance().refresh();
        }
    }

    javelin::jmap::cache::AccountReader& GuiServices::accountReader()
    {
        return *m_accountRepository;
    }
    javelin::jmap::cache::MailboxReader& GuiServices::mailboxReader()
    {
        return *m_mailboxRepository;
    }
    javelin::jmap::cache::ContactReader& GuiServices::contactReader()
    {
        return *m_contactRepository;
    }
    javelin::jmap::calendar::CalendarReader& GuiServices::calendarReader()
    {
        return *m_calendarReader;
    }
    javelin::jmap::contacts::ContactIdentityLookup& GuiServices::contactIdentityLookup()
    {
        return *m_contactIdentityLookup;
    }
    javelin::jmap::cache::IdentityReader& GuiServices::identityReader()
    {
        return *m_identityRepository;
    }
    javelin::jmap::cache::MessageViewReader& GuiServices::messageViewReader()
    {
        return *m_messageViewService;
    }
    javelin::jmap::cache::QueryReader& GuiServices::queryReader()
    {
        return *m_queryService;
    }
    AccountCommandPort& GuiServices::accountCommandPort()
    {
        return *m_accountCommands;
    }
    CalendarCommandPort& GuiServices::calendarCommandPort()
    {
        return *m_calendarCommands;
    }
    ComposeCommandPort& GuiServices::composeCommandPort()
    {
        return *m_composeCommands;
    }
    ContactCommandPort& GuiServices::contactCommandPort()
    {
        return *m_contactCommands;
    }
    MailCommandPort& GuiServices::mailCommandPort()
    {
        return *m_mailCommands;
    }
    SieveCommandPort& GuiServices::sieveCommandPort()
    {
        return *m_sieveCommands;
    }
    IdentityCommandPort& GuiServices::identityCommandPort()
    {
        return *m_identityCommands;
    }
    AccountRefreshPort& GuiServices::accountRefreshPort()
    {
        return *m_accountRefresh;
    }
    MessageContentPort& GuiServices::messageContentPort()
    {
        return *m_messageContent;
    }
    MessageListSessionFactoryPort& GuiServices::messageListSessionFactory()
    {
        return *m_messageListSessions;
    }
    MailApplicationEventsPort& GuiServices::mailEvents()
    {
        return *m_mailEvents;
    }
    MessageNavigationPort& GuiServices::messageNavigationPort()
    {
        return *m_messageNavigation;
    }
    UndoCommandPort& GuiServices::undoCommandPort()
    {
        return *m_undoCommands;
    }
    DeveloperDiagnosticsPort& GuiServices::developerDiagnosticsPort()
    {
        return *m_developerDiagnostics;
    }
    DeveloperMaintenancePort& GuiServices::developerMaintenancePort()
    {
        return *m_developerMaintenance;
    }
    javelin::gui::translation::TranslationService& GuiServices::translationService()
    {
        return *m_translationService;
    }
    WorkTaskPort& GuiServices::workTaskPort()
    {
        return *m_workTasks;
    }
    OnboardingPort& GuiServices::onboardingPort()
    {
        return *m_onboarding;
    }

    javelin::gui::settings::GuiSettings& GuiServices::settings()
    {
        return *m_settings;
    }
} // namespace javelin::app
