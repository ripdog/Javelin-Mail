#include "app/ApplicationBootstrap.h"

#include "app/ProcessServices.h"
#include "gui/shell/MainWindow.h"

#include <QApplication>

namespace javelin::app
{

    ApplicationBootstrap::ApplicationBootstrap(QApplication& application)
        : m_application(application), m_processServices(std::make_unique<ProcessServices>())
    {
    }

    ApplicationBootstrap::~ApplicationBootstrap() = default;

    int ApplicationBootstrap::run()
    {
        m_mainWindow =
            std::make_unique<javelin::gui::shell::MainWindow>(m_processServices->jmapCore());
        m_mainWindow->show();
        return m_application.exec();
    }

} // namespace javelin::app
