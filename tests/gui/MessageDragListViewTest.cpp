#include "gui/messages/MessageDragListView.h"

#include <QApplication>
#include <QPushButton>
#include <QStringListModel>
#include <QVBoxLayout>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("message list focus turns an existing current row into a visible selection",
          "[gui][messages][accessibility][focus]")
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* before = new QPushButton(QStringLiteral("Before"), &window);
    auto* view = new javelin::gui::messages::MessageDragListView(&window);
    QStringListModel model{{QStringLiteral("First"), QStringLiteral("Second")}, &window};
    view->setModel(&model);
    layout->addWidget(before);
    layout->addWidget(view);
    window.show();
    QApplication::processEvents();

    view->setCurrentIndex(model.index(0));
    view->selectionModel()->clearSelection();
    before->setFocus(Qt::OtherFocusReason);
    REQUIRE(view->selectionModel()->selectedRows().isEmpty());

    view->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();

    REQUIRE(view->selectionModel()->selectedRows().size() == 1);
    CHECK(view->selectionModel()->selectedRows().front() == model.index(0));
}
