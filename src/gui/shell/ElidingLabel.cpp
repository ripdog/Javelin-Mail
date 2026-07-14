#include "gui/shell/ElidingLabel.h"

#include <QFontMetrics>
#include <QResizeEvent>

namespace javelin::gui::shell
{
    ElidingLabel::ElidingLabel(QWidget* parent) : QLabel(parent)
    {
    }

    void ElidingLabel::setText(const QString& text)
    {
        m_fullText = text;
        updateElidedText();
    }

    QSize ElidingLabel::minimumSizeHint() const
    {
        auto result = QLabel::minimumSizeHint();
        result.setWidth(0);
        return result;
    }

    void ElidingLabel::resizeEvent(QResizeEvent* event)
    {
        QLabel::resizeEvent(event);
        updateElidedText();
    }

    void ElidingLabel::updateElidedText()
    {
        const auto elided =
            fontMetrics().elidedText(m_fullText, Qt::ElideRight, contentsRect().width());
        QLabel::setText(elided);
        setToolTip(elided == m_fullText ? QString{} : m_fullText);
    }
} // namespace javelin::gui::shell
