#pragma once

#include <QDialog>

class QTableView;

namespace javelin::app
{
    class WorkTaskPort;
    class WorkTaskModel;
} // namespace javelin::app

namespace javelin::gui::tasks
{
    class TaskCenterDialog final : public QDialog
    {
        Q_OBJECT

      public:
        explicit TaskCenterDialog(javelin::app::WorkTaskPort& taskPort, QWidget* parent = nullptr);

      private:
        javelin::app::WorkTaskModel* m_model = nullptr;
        QTableView* m_table = nullptr;
    };
} // namespace javelin::gui::tasks
