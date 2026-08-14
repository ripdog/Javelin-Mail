#pragma once

#include <QDialog>
#include <QTimer>

class QLabel;
class QProgressBar;
class QPushButton;

namespace javelin::app
{
    class ComposeCommandPort;
}

namespace javelin::gui::compose
{
    class UndoSendDialog final : public QDialog
    {
        Q_OBJECT

      public:
        UndoSendDialog(QString sendId, QString title, QString message,
                       qint64 deadlineEpochMilliseconds,
                       javelin::app::ComposeCommandPort& composeCommands,
                       QWidget* parent = nullptr);

        [[nodiscard]] const QString& sendId() const;

      private:
        void updateCountdown();
        void undoSend();

        QString m_sendId;
        qint64 m_deadlineEpochMilliseconds = 0;
        qint64 m_durationMilliseconds = 0;
        javelin::app::ComposeCommandPort& m_composeCommands;
        QLabel* m_countdown = nullptr;
        QProgressBar* m_progress = nullptr;
        QPushButton* m_undo = nullptr;
        QTimer m_timer;
        bool m_cancelling = false;
    };
} // namespace javelin::gui::compose
