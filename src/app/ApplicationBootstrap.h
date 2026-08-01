#pragma once

#include <QPointer>
#include <memory>

class QApplication;

namespace javelin::app
{
    class DaemonBootstrap;
    class DaemonBackgroundController;
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
        void createMainWindow();
        void reloadAccountSynchronizationSettings();
        void setupBackgroundActivation();
        void showTaskCenter();

        QApplication& m_application;
        std::unique_ptr<DaemonBootstrap> m_daemonBootstrap;
        std::unique_ptr<GuiServices> m_guiServices;
        std::unique_ptr<DaemonBackgroundController> m_backgroundController;
        QPointer<javelin::gui::shell::MainWindow> m_mainWindow;
        QPointer<javelin::gui::tasks::TaskCenterDialog> m_taskCenter;
    };

} // namespace javelin::app
