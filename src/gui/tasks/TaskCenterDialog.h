#pragma once

#include <QDialog>

class QPushButton;
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
        void updateActions();
        void pauseSelected();
        void resumeSelected();
        void retrySelected();

        javelin::app::WorkScheduler& m_scheduler;
        javelin::app::WorkTaskModel* m_model = nullptr;
        QTableView* m_table = nullptr;
        QPushButton* m_pauseButton = nullptr;
        QPushButton* m_resumeButton = nullptr;
        QPushButton* m_retryButton = nullptr;
    };
} // namespace javelin::gui::tasks
