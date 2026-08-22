#include "gui/messageview/MessageReaderCommandController.h"
#include "gui/messageview/HtmlMessageView.h"
#include "gui/messageview/MessageBodyPresenter.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include <memory>

TEST_CASE("reader command controller owns plain-text find traversal", "[qtwebengine]")
{
    QWidget dialogParent;
    QStackedWidget stack;
    QWidget placeholder;
    QScrollArea multiple;
    QTextBrowser plainText;
    javelin::gui::messageview::HtmlMessageView html;
    stack.addWidget(&placeholder);
    stack.addWidget(&multiple);
    stack.addWidget(&plainText);
    stack.addWidget(&html);

    javelin::gui::messageview::MessageBodyPresenter bodyPresenter{stack, placeholder, multiple,
                                                                  plainText, html};
    bodyPresenter.setActiveView(javelin::gui::messageview::MessageBodyPresenter::View::PlainText);
    plainText.setPlainText(QStringLiteral("alpha beta alpha"));

    QWidget findBar;
    QLineEdit findEdit;
    QLabel findResult;
    QToolButton previous;
    QToolButton next;
    bool contentAvailable = true;
    bool focusRestored = false;

    javelin::gui::messageview::MessageReaderCommandController controller{
        bodyPresenter,
        plainText,
        html,
        findBar,
        findEdit,
        findResult,
        previous,
        next,
        [&contentAvailable] { return contentAvailable; },
        [&focusRestored] { focusRestored = true; },
        [] { return QStringLiteral("Subject"); },
        dialogParent};

    CHECK(controller.available());
    controller.showFindBar();
    CHECK_FALSE(findBar.isHidden());

    findEdit.setText(QStringLiteral("alpha"));
    CHECK(findResult.text() == QStringLiteral("1 of 2"));
    CHECK(previous.isEnabled());
    CHECK(next.isEnabled());

    controller.findNext();
    CHECK(findResult.text() == QStringLiteral("2 of 2"));
    controller.findNext();
    CHECK(findResult.text() == QStringLiteral("1 of 2"));
    controller.findPrevious();
    CHECK(findResult.text() == QStringLiteral("2 of 2"));

    controller.resetFind(false);
    CHECK(findEdit.text().isEmpty());
    CHECK(findResult.text().isEmpty());
    CHECK_FALSE(findBar.isHidden());
    CHECK_FALSE(previous.isEnabled());
    CHECK_FALSE(next.isEnabled());

    controller.dismissFindBar();
    CHECK(findBar.isHidden());
    CHECK(focusRestored);

    contentAvailable = false;
    CHECK_FALSE(controller.available());
    controller.showFindBar();
    CHECK(findBar.isHidden());
}

TEST_CASE("reader HTML find callback tolerates destroyed controller and view", "[qtwebengine]")
{
    QWidget dialogParent;
    QStackedWidget stack;
    QWidget placeholder;
    QScrollArea multiple;
    QTextBrowser plainText;
    auto html = std::make_unique<javelin::gui::messageview::HtmlMessageView>();
    stack.addWidget(&placeholder);
    stack.addWidget(&multiple);
    stack.addWidget(&plainText);
    stack.addWidget(html.get());

    javelin::gui::messageview::MessageBodyPresenter bodyPresenter{stack, placeholder, multiple,
                                                                  plainText, *html};
    bodyPresenter.setActiveView(javelin::gui::messageview::MessageBodyPresenter::View::Html);
    html->setDocumentHtml("<p>alpha beta alpha</p>", "find-lifetime");

    QWidget findBar;
    QLineEdit findEdit;
    QLabel findResult;
    QToolButton previous;
    QToolButton next;
    auto controller = std::make_unique<javelin::gui::messageview::MessageReaderCommandController>(
        bodyPresenter, plainText, *html, findBar, findEdit, findResult, previous, next,
        [] { return true; }, [] {}, [] { return QStringLiteral("Subject"); }, dialogParent);

    controller->showFindBar();
    findEdit.setText(QStringLiteral("alpha"));
    html.reset();
    controller.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    SUCCEED();
}
