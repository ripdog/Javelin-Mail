#pragma once

#include <QStatusBar>

class QTimer;

namespace javelin::gui::shell
{

    class LayeredStatusBar final : public QStatusBar
    {
        Q_OBJECT

      public:
        explicit LayeredStatusBar(QWidget* parent = nullptr);

        void showMessage(const QString& message, int timeoutMs = 0);
        void clearMessage();
        void setOverlayMessage(QString message);

      private:
        void updateVisibleMessage();

        QTimer* m_backgroundTimer = nullptr;
        QString m_backgroundMessage;
        QString m_overlayMessage;
    };

} // namespace javelin::gui::shell
