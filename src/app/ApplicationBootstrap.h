#pragma once

#include <memory>

class QApplication;

namespace javelin::app
{
    class ProcessServices;
}

namespace javelin::gui::shell
{
    class MainWindow;
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
        QApplication& m_application;
        std::unique_ptr<ProcessServices> m_processServices;
        std::unique_ptr<javelin::gui::shell::MainWindow> m_mainWindow;
    };

} // namespace javelin::app
