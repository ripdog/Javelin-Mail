#include "app/ApplicationBootstrap.h"

#include "app/AddressSuggestionStore.h"
#include "app/ApplicationErrorCoordinator.h"
#include "app/CalendarNotificationService.h"
#include "app/DeferredSendService.h"
#include "app/DesktopNotificationController.h"
#include "app/FullMailSyncService.h"
#include "app/LocalMaintenanceService.h"
#include "app/MailApplicationService.h"
#include "app/MailIndexService.h"
#include "app/MessageNavigationCoordinator.h"
#include "app/ProcessServices.h"
#include "app/WorkScheduler.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/MainWindow.h"
#include "gui/tasks/TaskCenterDialog.h"

#include <KAboutData>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QNetworkInformation>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QToolButton>

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
        : m_application(application), m_processServices(std::make_unique<ProcessServices>()),
          m_notificationController(std::make_unique<DesktopNotificationController>())
    {
    }

    ApplicationBootstrap::~ApplicationBootstrap() = default;

    int ApplicationBootstrap::run()
    {
        KAboutData aboutData(QStringLiteral("javelinmail"), QStringLiteral("Javelin Mail"),
                             QStringLiteral(JAVELIN_APP_VERSION),
                             QStringLiteral("A JMAP email client"), KAboutLicense::GPL_V3);
        aboutData.setOrganizationDomain("javelin.app");
        KAboutData::setApplicationData(aboutData);

        m_application.setWindowIcon(QIcon(QStringLiteral(":/icons/icon.svg")));
        m_application.setQuitOnLastWindowClosed(false);

        const auto accounts = javelin::gui::settings::PreferencesDialog::loadAccounts();
        const bool hasUsableConnection = std::ranges::any_of(
            accounts, [](const auto& settings)
            { return !settings.loginEmail.isEmpty() && !settings.apiKey.isEmpty(); });
        if (!hasUsableConnection)
        {
            javelin::gui::settings::PreferencesDialog dialog{m_processServices->accountRepository(),
                                                             m_processServices->queryService()};
            dialog.exec();
        }

        reloadAccountSynchronizationSettings();
        setupNetworkReachability();
        setupSystemTray();
        m_processServices->deferredSendService().start();
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
            m_processServices->accountRepository(), m_processServices->contactRepository(),
            m_processServices->calendarService(), m_processServices->contactIdentityLookup(),
            m_processServices->identityRepository(), m_processServices->messageViewService(),
            m_processServices->queryService(), m_processServices->translationService(),
            m_processServices->composeService(), m_processServices->contactCommandPort(),
            m_processServices->mailService(), m_processServices->messageNavigationCoordinator(),
            m_processServices->undoManager());

        m_mainWindow->setAttribute(Qt::WA_DeleteOnClose);
        auto* taskButton = new QToolButton(m_mainWindow);
        taskButton->setAutoRaise(true);
        taskButton->setToolTip(QStringLiteral("Open Task Center"));
        m_mainWindow->statusBar()->addPermanentWidget(taskButton);
        const auto updateTaskButton = [this, taskButton]()
        {
            const QString summary = m_processServices->workScheduler().summary();
            taskButton->setText(summary);
            taskButton->setVisible(!summary.isEmpty());
        };
        QObject::connect(taskButton, &QToolButton::clicked, [this]() { showTaskCenter(); });
        QObject::connect(&m_processServices->workScheduler(), &WorkScheduler::jobsChanged,
                         taskButton, updateTaskButton);
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
        m_processServices->mailService().applySettings(std::move(configurations));
        m_processServices->fullMailSyncService().applySettings(std::move(fullSync));
        m_processServices->mailIndexService().applyAccounts(std::move(accountIds));
        m_processServices->localMaintenanceService().requestReplay();
    }

    void ApplicationBootstrap::setupNetworkReachability()
    {
        if (!QNetworkInformation::loadDefaultBackend())
        {
            qWarning() << QStringLiteral(
                "Network reachability backend unavailable; resume watchdog remains active");
            return;
        }

        auto* networkInformation = QNetworkInformation::instance();
        if (networkInformation == nullptr)
            return;

        QObject::connect(networkInformation, &QNetworkInformation::reachabilityChanged,
                         &m_application,
                         [this](const QNetworkInformation::Reachability reachability)
                         {
                             if (reachability != QNetworkInformation::Reachability::Online)
                                 return;
                             qInfo() << QStringLiteral(
                                 "Network became reachable; reconnecting account synchronization");
                             m_processServices->mailService().networkBecameReachable();
                         });
    }

    void ApplicationBootstrap::toggleMainWindow()
    {
        if (m_mainWindow)
        {
            m_mainWindow->close();
        }
        else
        {
            restoreMainWindow();
        }
    }

    void ApplicationBootstrap::setupSystemTray()
    {
        m_trayIcon = std::make_unique<QSystemTrayIcon>(QIcon(QStringLiteral(":/icons/icon.svg")));
        m_trayMenu = std::make_unique<QMenu>();

        auto* toggleAction = m_trayMenu->addAction(QStringLiteral("Toggle Javelin"));
        QObject::connect(toggleAction, &QAction::triggered, [&]() { toggleMainWindow(); });

        auto* taskCenterAction = m_trayMenu->addAction(QStringLiteral("Task Center…"));
        QObject::connect(taskCenterAction, &QAction::triggered, [this]() { showTaskCenter(); });

        m_trayMenu->addSeparator();

        auto* quitAction = m_trayMenu->addAction(QStringLiteral("Quit"));
        QObject::connect(quitAction, &QAction::triggered, [&]() { m_application.quit(); });

        m_trayIcon->setContextMenu(m_trayMenu.get());
        QObject::connect(
            &m_processServices->workScheduler(), &WorkScheduler::jobsChanged, &m_application,
            [this]()
            {
                const QString summary = m_processServices->workScheduler().summary();
                m_trayIcon->setToolTip(summary.isEmpty()
                                           ? QStringLiteral("Javelin Mail")
                                           : QStringLiteral("Javelin Mail — %1").arg(summary));
            });

        QObject::connect(
            &m_processServices->mailService(), &MailApplicationService::notificationRaised,
            m_notificationController.get(),
            [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                   const QString& emailId, const QString& mailboxName, const QString& title,
                   const QString& message)
            {
                static_cast<void>(this);
                m_notificationController->notifyNewMail(accountId, mailboxId, threadId, emailId,
                                                        mailboxName, title, message);
            });
        QObject::connect(&m_processServices->mailService(), &MailApplicationService::cacheCommitted,
                         &AddressSuggestionStore::instance(), &AddressSuggestionStore::refresh);
        QObject::connect(&m_processServices->mailService(), &MailApplicationService::cacheCommitted,
                         &m_application,
                         [this](const MailCacheChange& change)
                         {
                             m_processServices->localMaintenanceService().requestReplay();
                             if (!change.optimisticProjection)
                             {
                                 m_processServices->fullMailSyncService().requestCatchUp(
                                     change.accountId.toStdString());
                             }
                             if (change.hasNewMail)
                             {
                                 m_processServices->mailIndexService().requestIndex(
                                     change.accountId.toStdString());
                             }
                         });
        QObject::connect(
            &m_processServices->errorCoordinator(), &ApplicationErrorCoordinator::incidentRaised,
            m_notificationController.get(),
            [this](const QString& connectionId, const QString&, const QString& title,
                   const QString& message, const bool persistent, const bool opensSettings)
            {
                m_notificationController->notifyError(connectionId, title, message, persistent,
                                                      opensSettings);
            });
        QObject::connect(
            m_notificationController.get(), &DesktopNotificationController::notificationActivated,
            &m_application,
            [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                   const QString& emailId, const QString& activationToken)
            {
                restoreMainWindow(activationToken);
                const auto thread = threadId.isEmpty()
                                        ? std::optional<std::string>{std::nullopt}
                                        : std::optional<std::string>{threadId.toStdString()};
                static_cast<void>(m_processServices->messageNavigationCoordinator().openEmail(
                    accountId.toStdString(), mailboxId.toStdString(), thread,
                    emailId.toStdString()));
            });
        QObject::connect(&m_processServices->calendarNotificationService(),
                         &CalendarNotificationService::reminderDue, m_notificationController.get(),
                         &DesktopNotificationController::notifyCalendarEvent);
        QObject::connect(m_notificationController.get(),
                         &DesktopNotificationController::calendarNotificationAction, &m_application,
                         [this](const QString& key, const bool snooze)
                         {
                             if (snooze)
                                 m_processServices->calendarNotificationService().snooze(key);
                             else
                                 m_processServices->calendarNotificationService().dismiss(key);
                         });
        m_processServices->calendarNotificationService().start();
        QObject::connect(m_notificationController.get(),
                         &DesktopNotificationController::errorNotificationActivated, &m_application,
                         [this](const QString& connectionId, const QString& activationToken)
                         {
                             restoreMainWindow(activationToken);
                             if (m_mainWindow != nullptr)
                                 m_mainWindow->openPreferencesForConnection(connectionId);
                         });
        QObject::connect(
            &m_processServices->deferredSendService(), &DeferredSendService::undoableSendScheduled,
            m_notificationController.get(), &DesktopNotificationController::notifyUndoableSend);
        QObject::connect(
            &m_processServices->deferredSendService(), &DeferredSendService::undoableSendWaiting,
            m_notificationController.get(),
            [this](const QString& sendId, const QString& title, const QString& message)
            { m_notificationController->notifyUndoableSend(sendId, title, message, 0); });
        QObject::connect(&m_processServices->deferredSendService(),
                         &DeferredSendService::undoableSendClosed, m_notificationController.get(),
                         &DesktopNotificationController::closeUndoableSendNotification);
        QObject::connect(m_notificationController.get(),
                         &DesktopNotificationController::undoSendRequested, &m_application,
                         [this](const QString& sendId)
                         {
                             static_cast<void>(
                                 m_processServices->deferredSendService().cancelTargeted(sendId));
                         });
        QObject::connect(&m_processServices->deferredSendService(),
                         &DeferredSendService::draftRestoreRequested, &m_application,
                         [this](const QString& accountId, const QString& draftEmailId,
                                const QString& composeSessionId)
                         {
                             restoreMainWindow({});
                             if (m_mainWindow != nullptr)
                                 m_mainWindow->restoreDraft(accountId, draftEmailId,
                                                            composeSessionId);
                         });
        QObject::connect(&m_processServices->deferredSendService(),
                         &DeferredSendService::sendFailed, m_notificationController.get(),
                         [this](const QString&, const QString& message)
                         {
                             m_notificationController->notifyError(
                                 {}, QStringLiteral("Unable to send message"), message, true,
                                 false);
                         });

        QObject::connect(m_trayIcon.get(), &QSystemTrayIcon::activated,
                         [&](QSystemTrayIcon::ActivationReason reason)
                         {
                             if (reason == QSystemTrayIcon::Trigger ||
                                 reason == QSystemTrayIcon::DoubleClick)
                             {
                                 toggleMainWindow();
                             }
                         });

        m_trayIcon->show();
    }

    void ApplicationBootstrap::showTaskCenter()
    {
        if (m_taskCenter == nullptr)
            m_taskCenter = new javelin::gui::tasks::TaskCenterDialog(
                m_processServices->workScheduler(), m_mainWindow);
        m_taskCenter->show();
        m_taskCenter->raise();
        m_taskCenter->activateWindow();
    }

} // namespace javelin::app
