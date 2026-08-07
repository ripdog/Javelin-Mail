#pragma once

#include <QEvent>
#include <QProgressBar>

namespace javelin::gui::widgets
{
    class IndeterminateProgressBar final : public QProgressBar
    {
      public:
        using QProgressBar::QProgressBar;

      protected:
        bool event(QEvent* event) override
        {
            // Qt 6.11's QStyleSheetStyle starts a QProgressStyleAnimation for a styled (0, 0)
            // progress bar. Its determinate cleanup is incorrectly guarded by chunkWidth > 0, so
            // returning to value 0 leaves that animation registered forever. QStyleAnimation stops
            // itself when its StyleAnimationUpdate event is not accepted, which is also why hiding
            // the widget clears the repaint loop. Reject the first stale update after leaving busy
            // mode while leaving real indeterminate animation completely untouched.
            if (event->type() == QEvent::StyleAnimationUpdate && (minimum() != 0 || maximum() != 0))
            {
                event->ignore();
                return true;
            }

            return QProgressBar::event(event);
        }
    };
} // namespace javelin::gui::widgets
