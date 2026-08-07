#include "gui/widgets/IndeterminateProgressBar.h"

#include <QApplication>
#include <QEvent>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("styled indeterminate progress bars reject stale style animation updates")
{
    javelin::gui::widgets::IndeterminateProgressBar progressBar;
    progressBar.setStyleSheet(
        QStringLiteral("QProgressBar { border: none; background: transparent; }"
                       "QProgressBar::chunk { background-color: palette(highlight); }"));
    progressBar.show();
    QApplication::processEvents();

    progressBar.setRange(0, 0);
    QEvent busyUpdate{QEvent::StyleAnimationUpdate};
    busyUpdate.ignore();
    QApplication::sendEvent(&progressBar, &busyUpdate);
    CHECK(busyUpdate.isAccepted());

    progressBar.setRange(0, 1);
    progressBar.setValue(0);
    QEvent staleUpdate{QEvent::StyleAnimationUpdate};
    staleUpdate.ignore();
    QApplication::sendEvent(&progressBar, &staleUpdate);
    CHECK_FALSE(staleUpdate.isAccepted());

    progressBar.setRange(0, 0);
    QEvent restartedBusyUpdate{QEvent::StyleAnimationUpdate};
    restartedBusyUpdate.ignore();
    QApplication::sendEvent(&progressBar, &restartedBusyUpdate);
    CHECK(restartedBusyUpdate.isAccepted());
}
