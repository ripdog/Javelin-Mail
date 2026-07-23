#include "gui/shell/LayeredStatusBar.h"

#include <QTimer>

#include <utility>

namespace javelin::gui::shell
{

    LayeredStatusBar::LayeredStatusBar(QWidget* parent) : QStatusBar(parent)
    {
        m_backgroundTimer = new QTimer(this);
        m_backgroundTimer->setSingleShot(true);
        connect(m_backgroundTimer, &QTimer::timeout, this,
                [this]
                {
                    m_backgroundMessage.clear();
                    updateVisibleMessage();
                });
    }

    void LayeredStatusBar::showMessage(const QString& message, const int timeoutMs)
    {
        m_backgroundMessage = message;
        if (timeoutMs > 0)
        {
            m_backgroundTimer->start(timeoutMs);
        }
        else
        {
            m_backgroundTimer->stop();
        }
        updateVisibleMessage();
    }

    void LayeredStatusBar::setOverlayMessage(QString message)
    {
        m_overlayMessage = std::move(message);
        updateVisibleMessage();
    }

    void LayeredStatusBar::updateVisibleMessage()
    {
        QStatusBar::showMessage(m_overlayMessage.isEmpty() ? m_backgroundMessage
                                                           : m_overlayMessage);
    }

} // namespace javelin::gui::shell
