#pragma once

#include <memory>
#include <QPointer>

class QApplication;

namespace javelin::app
{
    class DesktopNotificationController;
    class ProcessServices;
}

namespace javelin::gui::shell
{
    class MainWindow;
}

class QSystemTrayIcon;
class QMenu;

namespace javelin::app
{

    class ApplicationBootstrap
    {
      public:
        explicit ApplicationBootstrap(QApplication& application);
        ~ApplicationBootstrap();

        ApplicationBootstrap(const ApplicationBootstrap&) = delete;
        ApplicationBootstrap& operator=(const ApplicationBootstrap&) = delete;
        ApplicationBootstrap(ApplicationBootstrap&&) = delete;
        ApplicationBootstrap& operator=(ApplicationBootstrap&&) = delete;

        [[nodiscard]] int run();

      private:
        void restoreMainWindow(const QString& activationToken = {});
        void toggleMainWindow();
        void createMainWindow();
        void setupSystemTray();

        QApplication& m_application;
        std::unique_ptr<ProcessServices> m_processServices;
        std::unique_ptr<DesktopNotificationController> m_notificationController;
        QPointer<javelin::gui::shell::MainWindow> m_mainWindow;
        std::unique_ptr<QSystemTrayIcon> m_trayIcon;
        std::unique_ptr<QMenu> m_trayMenu;
    };

} // namespace javelin::app
