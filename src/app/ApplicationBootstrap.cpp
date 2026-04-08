#include "app/ApplicationBootstrap.h"

#include "app/DesktopNotificationController.h"
#include "app/LongPollService.h"
#include "app/ProcessServices.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/MainWindow.h"

#include <KAboutData>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace javelin::app
{

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

        [[nodiscard]] javelin::jmap::LiveConnectionSettings
        toLiveConnectionSettings(const javelin::gui::settings::ConnectionSettings& settings)
        {
            return javelin::jmap::LiveConnectionSettings{
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
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
                             QStringLiteral("0.1.0"), QStringLiteral("A JMAP email client"),
                             KAboutLicense::GPL_V3);
        aboutData.setOrganizationDomain("javelin.app");
        KAboutData::setApplicationData(aboutData);

        m_application.setQuitOnLastWindowClosed(false);
        setupSystemTray();
        createMainWindow();

        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettings();
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            javelin::gui::settings::PreferencesDialog dialog{m_mainWindow};
            dialog.exec();
        }
        else
        {
            m_processServices->longPollService().applySettings(toLiveConnectionSettings(settings));
        }

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
            m_processServices->jmapCore(), m_processServices->accountRepository(),
            m_processServices->messageViewService(), m_processServices->queryService(),
            m_processServices->longPollService());

        m_mainWindow->setAttribute(Qt::WA_DeleteOnClose);

        m_mainWindow->show();
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

        m_trayMenu->addSeparator();

        auto* quitAction = m_trayMenu->addAction(QStringLiteral("Quit"));
        QObject::connect(quitAction, &QAction::triggered, [&]() { m_application.quit(); });

        m_trayIcon->setContextMenu(m_trayMenu.get());

        QObject::connect(
            &m_processServices->longPollService(), &LongPollService::notificationRaised,
            m_notificationController.get(),
            [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                   const QString& emailId, const QString& mailboxName, const QString& title,
                   const QString& message)
            {
                static_cast<void>(this);
                m_notificationController->notifyNewMail(accountId, mailboxId, threadId, emailId,
                                                        mailboxName, title, message);
            });
        QObject::connect(
            m_notificationController.get(), &DesktopNotificationController::notificationActivated,
            &m_application,
            [this](const QString& accountId, const QString& mailboxId, const QString& threadId,
                   const QString& emailId, const QString& activationToken)
            {
                restoreMainWindow(activationToken);
                if (m_mainWindow != nullptr)
                {
                    m_mainWindow->openMessageFromNotification(accountId, mailboxId, threadId,
                                                              emailId);
                }
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

} // namespace javelin::app
