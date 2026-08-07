#pragma once

#include <QPainter>
#include <QProgressBar>
#include <QStyle>
#include <QStyleOptionProgressBar>

namespace javelin::gui::widgets
{
    class StaticIndeterminateProgressBar final : public QProgressBar
    {
      public:
        using QProgressBar::QProgressBar;

      protected:
        void paintEvent(QPaintEvent* event) override
        {
            if (minimum() != 0 || maximum() != 0)
            {
                QProgressBar::paintEvent(event);
                return;
            }

            Q_UNUSED(event)
            QStyleOptionProgressBar option;
            initStyleOption(&option);

            // QStyleSheetStyle creates a QStyleAnimation whenever it paints a real (0, 0)
            // progress range. In a top-level window containing WebEngine that animation forces the
            // shared QWidget backing store through the RHI compositor every frame. Paint the busy
            // state as a fixed partial chunk instead: it remains an actual QProgressBar with the
            // application's normal style, but no free-running style animation is created.
            option.minimum = 0;
            option.maximum = 3;
            option.progress = 1;
            option.textVisible = false;

            QPainter painter{this};
            style()->drawControl(QStyle::CE_ProgressBar, &option, &painter, this);
        }
    };
} // namespace javelin::gui::widgets
