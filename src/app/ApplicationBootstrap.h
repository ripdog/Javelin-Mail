#pragma once

#include <QPointer>
#include <memory>

class QApplication;

namespace javelin::app
{
    class DaemonBootstrap;
    class DesktopNotificationController;
    class DaemonServices;
    class GuiServices;
} // namespace javelin::app

namespace javelin::gui::shell
{
    class MainWindow;
}
namespace javelin::gui::tasks
{
    class TaskCenterDialog;
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
        [[nodiscard]] DaemonServices& daemonServices();
        [[nodiscard]] GuiServices& guiServices();
        void restoreMainWindow(const QString& activationToken = {});
        void toggleMainWindow();
        void createMainWindow();
        void reloadAccountSynchronizationSettings();
        void setupNetworkReachability();
        void setupSystemTray();
        void showTaskCenter();

        QApplication& m_application;
        std::unique_ptr<DaemonBootstrap> m_daemonBootstrap;
        std::unique_ptr<GuiServices> m_guiServices;
        std::unique_ptr<DesktopNotificationController> m_notificationController;
        QPointer<javelin::gui::shell::MainWindow> m_mainWindow;
        std::unique_ptr<QSystemTrayIcon> m_trayIcon;
        std::unique_ptr<QMenu> m_trayMenu;
        QPointer<javelin::gui::tasks::TaskCenterDialog> m_taskCenter;
    };

} // namespace javelin::app
