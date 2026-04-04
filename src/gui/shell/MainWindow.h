#pragma once

#include <QMainWindow>

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::gui::shell
{

    class MainWindow : public QMainWindow
    {
      public:
        explicit MainWindow(javelin::jmap::JmapCore& jmapCore, QWidget* parent = nullptr);
        ~MainWindow() override = default;

      private:
        void setupUi();

        javelin::jmap::JmapCore& m_jmapCore;
    };

} // namespace javelin::gui::shell
