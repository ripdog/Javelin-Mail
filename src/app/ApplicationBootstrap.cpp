#include "app/ApplicationBootstrap.h"

#include "app/AddressSuggestionStore.h"
#include "app/DaemonBackgroundController.h"
#include "app/DaemonBootstrap.h"
#include "app/DaemonServices.h"
#include "app/FullMailSyncService.h"
#include "app/GuiServices.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationService.h"
#include "app/MailIndexService.h"
#include "app/MessageNavigationPort.h"
#include "app/TranslationApplicationPorts.h"
#include "app/WorkScheduler.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/MainWindow.h"
#include "gui/tasks/TaskCenterDialog.h"
#include "protocol/ProcessBoundary.h"

#include <KAboutData>
#include <QApplication>
#include <QByteArray>
#include <QGuiApplication>
#include <QIcon>
#include <QStatusBar>
#include <QToolButton>

#include <type_traits>
#include <variant>

namespace javelin::app
{

#ifndef JAVELIN_APP_VERSION
#define JAVELIN_APP_VERSION "0.0.0"
#endif

    namespace
    {
        class ActivationTokenScope final
        {
          public:
            explicit ActivationTokenScope(const QString& activationToken)
            {
                if (!QGuiApplication::platformName().startsWith(QStringLiteral("wayland"),
                                                                Qt::CaseInsensitive) ||
                    activationToken.isEmpty())
                {
                    return;
                }

                m_active = true;
                m_hadPreviousToken = qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN");
                if (m_hadPreviousToken)
                {
                    m_previousToken = qgetenv("XDG_ACTIVATION_TOKEN");
                }
                qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
            }

            ~ActivationTokenScope()
            {
                if (!m_active)
                {
                    return;
                }

                if (m_hadPreviousToken)
                {
                    qputenv("XDG_ACTIVATION_TOKEN", m_previousToken);
                }
                else
                {
                    qunsetenv("XDG_ACTIVATION_TOKEN");
                }
            }

          private:
            bool m_active = false;
            bool m_hadPreviousToken = false;
            QByteArray m_previousToken;
        };

        [[nodiscard]] AccountConnectionSettings
        toAccountConnectionSettings(const javelin::gui::settings::ConnectionSettings& settings)
        {
            return AccountConnectionSettings{
                .connectionId = settings.id.toStdString(),
                .revision = settings.revision,
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

        [[nodiscard]] std::vector<AccountSyncConfiguration> accountSyncConfigurations()
        {
            std::vector<AccountSyncConfiguration> configurations;
            for (const auto& settings : javelin::gui::settings::PreferencesDialog::loadAccounts())
            {
                if (settings.loginEmail.isEmpty() || settings.apiKey.isEmpty())
                {
                    continue;
                }
                for (const auto& accountId : settings.cachedAccountIds)
                {
                    std::vector<std::string> mailboxIds;
                    for (const auto& mailboxId :
                         javelin::gui::settings::PreferencesDialog::syncedMailboxIds(accountId))
                    {
                        mailboxIds.push_back(mailboxId.toStdString());
                    }
                    std::vector<std::string> notificationMailboxIds;
                    for (const auto& mailboxId :
                         javelin::gui::settings::PreferencesDialog::notificationMailboxIds(
                             accountId))
                    {
                        notificationMailboxIds.push_back(mailboxId.toStdString());
                    }
                    mailboxIds.insert(mailboxIds.end(), notificationMailboxIds.begin(),
                                      notificationMailboxIds.end());
                    std::ranges::sort(mailboxIds);
                    mailboxIds.erase(std::ranges::unique(mailboxIds).begin(), mailboxIds.end());
                    configurations.push_back(AccountSyncConfiguration{
                        .settings = toAccountConnectionSettings(settings),
                        .accountId = accountId.toStdString(),
                        .mailboxIds = std::move(mailboxIds),
                        .fullSyncMailboxIds =
                            [&]()
                        {
                            std::vector<std::string> ids;
                            for (const auto& mailboxId :
                                 javelin::gui::settings::PreferencesDialog::syncedMailboxIds(
                                     accountId))
                                ids.push_back(mailboxId.toStdString());
                            return ids;
                        }(),
                        .notificationMailboxIds = std::move(notificationMailboxIds),
                        .notificationMailboxSelectionConfigured = javelin::gui::settings::
                            PreferencesDialog::hasNotificationMailboxSelection(accountId),
                    });
                }
            }
            return configurations;
        }

    } // namespace

    ApplicationBootstrap::ApplicationBootstrap(QApplication& application)
        : m_application(application), m_daemonBootstrap(std::make_unique<DaemonBootstrap>()),
          m_guiServices(
              std::make_unique<GuiServices>(m_daemonBootstrap->services().databasePath(),
                                            m_daemonBootstrap->services().cacheAccessBarrier(),
                                            m_daemonBootstrap->services().contactRepository())),
          m_backgroundController(
              std::make_unique<DaemonBackgroundController>(m_daemonBootstrap->services()))
    {
    }

    ApplicationBootstrap::~ApplicationBootstrap() = default;

    DaemonServices& ApplicationBootstrap::daemonServices()
    {
        return m_daemonBootstrap->services();
    }

    GuiServices& ApplicationBootstrap::guiServices()
    {
        return *m_guiServices;
    }

    int ApplicationBootstrap::run()
    {
        KAboutData aboutData(QStringLiteral("javelinmail"), QStringLiteral("Javelin Mail"),
                             QStringLiteral(JAVELIN_APP_VERSION),
                             QStringLiteral("A JMAP email client"), KAboutLicense::GPL_V3);
        aboutData.setOrganizationDomain("javelin.app");
        KAboutData::setApplicationData(aboutData);
        // Process services are constructed before KAboutData finalizes the QSettings identity.
        daemonServices().translationService().reloadSettings();

        m_application.setWindowIcon(QIcon(QStringLiteral(":/icons/icon.svg")));
        m_application.setQuitOnLastWindowClosed(false);

        const auto accounts = javelin::gui::settings::PreferencesDialog::loadAccounts();
        const bool hasUsableConnection = std::ranges::any_of(
            accounts, [](const auto& settings)
            { return !settings.loginEmail.isEmpty() && !settings.apiKey.isEmpty(); });
        if (!hasUsableConnection)
        {
            javelin::gui::settings::PreferencesDialog dialog{daemonServices().accountCommandPort(),
                                                             guiServices().accountReader(),
                                                             guiServices().mailboxReader()};
            dialog.exec();
        }

        reloadAccountSynchronizationSettings();
        setupBackgroundActivation();
        QObject::connect(&daemonServices().mailService(), &MailApplicationService::cacheCommitted,
                         &guiServices().addressSuggestionStore(), &AddressSuggestionStore::refresh);
        m_backgroundController->start();
        createMainWindow();

        return m_application.exec();
    }

    void ApplicationBootstrap::restoreMainWindow(const QString& activationToken)
    {
        if (!m_mainWindow)
        {
            createMainWindow();
        }

        if (!m_mainWindow)
        {
            return;
        }

        ActivationTokenScope activationTokenScope{activationToken};
        m_mainWindow->show();
        if (m_mainWindow->isMinimized())
        {
            m_mainWindow->showNormal();
        }
        m_mainWindow->raise();
        m_mainWindow->activateWindow();
    }

    void ApplicationBootstrap::createMainWindow()
    {
        if (m_mainWindow)
        {
            restoreMainWindow();
            return;
        }

        m_mainWindow = new javelin::gui::shell::MainWindow(
            daemonServices().accountCommandPort(), guiServices().accountReader(),
            guiServices().mailboxReader(), guiServices().contactReader(),
            guiServices().calendarReader(), daemonServices().calendarCommandPort(),
            guiServices().contactIdentityLookup(), guiServices().identityReader(),
            guiServices().messageViewReader(), guiServices().queryReader(),
            daemonServices().translationService(), daemonServices().composeCommandPort(),
            daemonServices().contactCommandPort(), daemonServices().mailCommandPort(),
            daemonServices().sieveCommandPort(), daemonServices().accountRefreshPort(),
            daemonServices().messageContentPort(), daemonServices().messageListSessionFactory(),
            daemonServices().mailApplicationEvents(), daemonServices().messageNavigationPort(),
            daemonServices().undoCommandPort());

        m_mainWindow->setAttribute(Qt::WA_DeleteOnClose);
        auto* taskButton = new QToolButton(m_mainWindow);
        taskButton->setAutoRaise(true);
        taskButton->setToolTip(QStringLiteral("Open Task Center"));
        m_mainWindow->statusBar()->addPermanentWidget(taskButton);
        const auto updateTaskButton = [this, taskButton]()
        {
            const QString summary = daemonServices().workScheduler().summary();
            taskButton->setText(summary);
            taskButton->setVisible(!summary.isEmpty());
        };
        QObject::connect(taskButton, &QToolButton::clicked, [this]() { showTaskCenter(); });
        QObject::connect(&daemonServices().workScheduler(), &WorkScheduler::jobsChanged, taskButton,
                         updateTaskButton);
        updateTaskButton();
        QObject::connect(m_mainWindow, &javelin::gui::shell::MainWindow::accountSettingsChanged,
                         m_mainWindow, [this]() { reloadAccountSynchronizationSettings(); });

        m_mainWindow->show();
    }

    void ApplicationBootstrap::reloadAccountSynchronizationSettings()
    {
        auto configurations = accountSyncConfigurations();
        std::vector<FullSyncAccountConfiguration> fullSync;
        std::vector<std::string> accountIds;
        fullSync.reserve(configurations.size());
        for (const auto& configuration : configurations)
        {
            fullSync.push_back({.settings = configuration.settings,
                                .accountId = configuration.accountId,
                                .mailboxIds = configuration.fullSyncMailboxIds});
            accountIds.push_back(configuration.accountId);
        }
        daemonServices().mailService().applySettings(std::move(configurations));
        daemonServices().fullMailSyncService().applySettings(std::move(fullSync));
        daemonServices().mailIndexService().applyAccounts(std::move(accountIds));
        daemonServices().localMaintenanceService().requestReplay();
    }

    void ApplicationBootstrap::setupBackgroundActivation()
    {
        QObject::connect(
            m_backgroundController.get(), &DaemonBackgroundController::activationRequested,
            &m_application,
            [this](protocol::ActivationRoute route)
            {
                std::visit(
                    [this](const auto& activation)
                    {
                        using Route = std::decay_t<decltype(activation)>;
                        if constexpr (std::is_same_v<Route, protocol::OpenMessageRoute>)
                        {
                            restoreMainWindow(activation.activationToken);
                            const auto thread =
                                activation.threadId.isEmpty()
                                    ? std::optional<std::string>{std::nullopt}
                                    : std::optional<std::string>{activation.threadId.toStdString()};
                            static_cast<void>(daemonServices().messageNavigationPort().openEmail(
                                activation.accountId.toStdString(),
                                activation.mailboxId.toStdString(), thread,
                                activation.emailId.toStdString()));
                        }
                        else if constexpr (std::is_same_v<Route, protocol::OpenSettingsRoute>)
                        {
                            restoreMainWindow(activation.activationToken);
                            if (m_mainWindow != nullptr)
                                m_mainWindow->openPreferencesForConnection(activation.connectionId);
                        }
                        else if constexpr (std::is_same_v<Route, protocol::RestoreDraftRoute>)
                        {
                            restoreMainWindow(activation.activationToken);
                            if (m_mainWindow != nullptr)
                                m_mainWindow->restoreDraft(activation.accountId,
                                                           activation.draftEmailId,
                                                           activation.composeSessionId);
                        }
                        else if constexpr (std::is_same_v<Route, protocol::OpenTaskCenterRoute>)
                        {
                            restoreMainWindow(activation.activationToken);
                            showTaskCenter();
                        }
                        else if constexpr (requires { activation.activationToken; })
                            restoreMainWindow(activation.activationToken);
                        else
                            restoreMainWindow();
                    },
                    route);
            });
        QObject::connect(m_backgroundController.get(),
                         &DaemonBackgroundController::shutdownRequested, &m_application,
                         [this]() { m_application.quit(); });
    }

    void ApplicationBootstrap::showTaskCenter()
    {
        if (m_taskCenter == nullptr)
            m_taskCenter = new javelin::gui::tasks::TaskCenterDialog(
                daemonServices().workScheduler(), m_mainWindow);
        m_taskCenter->show();
        m_taskCenter->raise();
        m_taskCenter->activateWindow();
    }

} // namespace javelin::app
