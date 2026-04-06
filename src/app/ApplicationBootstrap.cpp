#include "app/ApplicationBootstrap.h"

#include "app/ProcessServices.h"
#include "gui/shell/MainWindow.h"

#include <QApplication>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QIcon>
#include <QAction>

namespace javelin::app
{

    ApplicationBootstrap::ApplicationBootstrap(QApplication& application)
        : m_application(application), m_processServices(std::make_unique<ProcessServices>())
    {
    }

    ApplicationBootstrap::~ApplicationBootstrap() = default;

    int ApplicationBootstrap::run()
    {
        m_application.setQuitOnLastWindowClosed(false);
        setupSystemTray();
        createMainWindow();
        return m_application.exec();
    }

    void ApplicationBootstrap::createMainWindow()
    {
        if (m_mainWindow) {
            m_mainWindow->show();
            m_mainWindow->raise();
            m_mainWindow->activateWindow();
            return;
        }

        m_mainWindow = new javelin::gui::shell::MainWindow(
            m_processServices->jmapCore(), m_processServices->accountRepository(),
            m_processServices->messageViewService(), m_processServices->queryService());
        
        m_mainWindow->setAttribute(Qt::WA_DeleteOnClose);

        m_mainWindow->show();
    }

    void ApplicationBootstrap::toggleMainWindow()
    {
        if (m_mainWindow) {
            m_mainWindow->close();
        } else {
            createMainWindow();
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
        
        QObject::connect(m_trayIcon.get(), &QSystemTrayIcon::activated, [&](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                toggleMainWindow();
            }
        });

        m_trayIcon->show();
    }

} // namespace javelin::app
