#pragma once

#include <QDialog>

class QTableView;

namespace javelin::app
{
    class WorkScheduler;
    class WorkTaskModel;
} // namespace javelin::app

namespace javelin::gui::tasks
{
    class TaskCenterDialog final : public QDialog
    {
        Q_OBJECT

      public:
        explicit TaskCenterDialog(javelin::app::WorkScheduler& scheduler,
                                  QWidget* parent = nullptr);

      private:
        javelin::app::WorkTaskModel* m_model = nullptr;
        QTableView* m_table = nullptr;
    };
} // namespace javelin::gui::tasks
