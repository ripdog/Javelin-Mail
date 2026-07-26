#include "gui/shell/FocusedCommandRouter.h"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QApplication::instance() != nullptr)
                return;
            qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
            static int argc = 1;
            static char appName[] = "focused-command-router-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QApplication> m_application;
    };

} // namespace

TEST_CASE("focused command router prioritizes native editor history", "[gui][undo][focused-router]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    using javelin::gui::shell::EditHistoryDirection;
    using javelin::gui::shell::FocusedCommandRouter;

    QLineEdit lineEdit;
    lineEdit.insert(QStringLiteral("line"));
    REQUIRE(FocusedCommandRouter::isNativeCommandAvailable(&lineEdit, EditHistoryDirection::Undo));
    REQUIRE(FocusedCommandRouter::invokeNativeCommand(&lineEdit, EditHistoryDirection::Undo));
    CHECK(lineEdit.text().isEmpty());
    REQUIRE(FocusedCommandRouter::invokeNativeCommand(&lineEdit, EditHistoryDirection::Redo));
    CHECK(lineEdit.text() == QStringLiteral("line"));

    QTextEdit textEdit;
    textEdit.insertPlainText(QStringLiteral("rich"));
    REQUIRE(FocusedCommandRouter::invokeNativeCommand(&textEdit, EditHistoryDirection::Undo));
    CHECK(textEdit.toPlainText().isEmpty());

    QPlainTextEdit plainTextEdit;
    plainTextEdit.insertPlainText(QStringLiteral("plain"));
    REQUIRE(FocusedCommandRouter::invokeNativeCommand(&plainTextEdit, EditHistoryDirection::Undo));
    CHECK(plainTextEdit.toPlainText().isEmpty());

    QComboBox comboBox;
    comboBox.setEditable(true);
    comboBox.lineEdit()->insert(QStringLiteral("combo"));
    REQUIRE(FocusedCommandRouter::invokeNativeCommand(&comboBox, EditHistoryDirection::Undo));
    CHECK(comboBox.currentText().isEmpty());
}

TEST_CASE("focused command router declines widgets without native history",
          "[gui][undo][focused-router]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QWidget widget;
    CHECK_FALSE(javelin::gui::shell::FocusedCommandRouter::isNativeCommandAvailable(
        &widget, javelin::gui::shell::EditHistoryDirection::Undo));
    CHECK_FALSE(javelin::gui::shell::FocusedCommandRouter::invokeNativeCommand(
        &widget, javelin::gui::shell::EditHistoryDirection::Redo));
}
