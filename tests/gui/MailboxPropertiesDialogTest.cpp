#include "gui/mailboxes/MailboxPropertiesDialog.h"

#include <QApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

namespace
{
    [[nodiscard]] javelin::jmap::cache::MailboxTreeItem mailbox()
    {
        return {
            .id = "mailbox-1",
            .name = "Projects",
            .parentId = std::nullopt,
            .role = std::nullopt,
            .sortOrder = 42,
            .totalEmails = 0,
            .unreadEmails = 0,
            .totalThreads = 0,
            .unreadThreads = 0,
            .isSubscribed = true,
            .myRights =
                {
                    .mayReadItems = true,
                    .mayAddItems = true,
                    .mayRemoveItems = true,
                    .maySetSeen = true,
                    .maySetKeywords = true,
                    .mayCreateChild = true,
                    .mayRename = true,
                    .mayDelete = true,
                    .maySubmit = false,
                },
            .hasChildren = false,
        };
    }

    [[nodiscard]] QString dialogText(const QWidget& widget)
    {
        QStringList values;
        for (const auto* label : widget.findChildren<QLabel*>())
            values.push_back(label->text());
        return values.join(QLatin1Char('\n'));
    }
} // namespace

TEST_CASE("mailbox properties present user-facing details and allow safe deletion",
          "[gui][mailbox][properties]")
{
    const auto value = mailbox();
    javelin::gui::mailboxes::MailboxPropertiesDialog dialog{
        QStringLiteral("Personal"), {}, value, true, false};

    const auto text = dialogText(dialog);
    CHECK(text.contains(QStringLiteral("Projects")));
    CHECK(text.contains(QStringLiteral("Personal")));
    CHECK(text.contains(QStringLiteral("Regular mailbox")));
    CHECK(text.contains(QStringLiteral("Shown")));
    CHECK(text.contains(QStringLiteral("Available offline:")));
    CHECK(text.contains(QStringLiteral("New-mail notifications:")));
    CHECK(text.contains(QStringLiteral("42")));
    CHECK(text.contains(QStringLiteral("Read messages:")));
    CHECK(text.contains(QStringLiteral("Delete mailbox:")));
    CHECK(text.contains(QStringLiteral("No messages will be deleted")));

    auto* deleteButton = dialog.findChild<QPushButton*>(QStringLiteral("deleteMailboxButton"));
    REQUIRE(deleteButton != nullptr);
    CHECK(deleteButton->isEnabled());
    CHECK_FALSE(dialog.deleteRequested());
}

TEST_CASE("mailbox deletion confirmation identifies its account and parent",
          "[gui][mailbox][properties][delete]")
{
    auto value = mailbox();
    value.parentId = "parent-1";
    javelin::gui::mailboxes::MailboxPropertiesDialog dialog{
        QStringLiteral("Personal Account"), QStringLiteral("Inbox"), value, false, false};
    auto* deleteButton = dialog.findChild<QPushButton*>(QStringLiteral("deleteMailboxButton"));
    REQUIRE(deleteButton != nullptr);

    QString confirmationText;
    QTimer::singleShot(0, &dialog,
                       [&confirmationText]
                       {
                           auto* confirmation =
                               qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                           REQUIRE(confirmation != nullptr);
                           confirmationText = confirmation->text();
                           auto* cancel = confirmation->button(QMessageBox::Cancel);
                           REQUIRE(cancel != nullptr);
                           cancel->click();
                       });
    deleteButton->click();

    CHECK(confirmationText.contains(QStringLiteral("Projects")));
    CHECK(confirmationText.contains(QStringLiteral("Personal Account")));
    CHECK(confirmationText.contains(QStringLiteral("Inbox")));
    CHECK_FALSE(dialog.deleteRequested());
}

TEST_CASE("mailbox properties disable deletion until server preconditions are satisfied",
          "[gui][mailbox][properties][delete]")
{
    SECTION("mailbox contains messages")
    {
        auto value = mailbox();
        value.totalEmails = 2;
        javelin::gui::mailboxes::MailboxPropertiesDialog dialog{
            QStringLiteral("Personal"), {}, value, false, false};
        auto* deleteButton = dialog.findChild<QPushButton*>(QStringLiteral("deleteMailboxButton"));
        REQUIRE(deleteButton != nullptr);
        CHECK_FALSE(deleteButton->isEnabled());
        CHECK(deleteButton->toolTip().contains(QStringLiteral("2 messages")));
    }

    SECTION("mailbox has children")
    {
        auto value = mailbox();
        value.hasChildren = true;
        javelin::gui::mailboxes::MailboxPropertiesDialog dialog{
            QStringLiteral("Personal"), {}, value, false, false};
        auto* deleteButton = dialog.findChild<QPushButton*>(QStringLiteral("deleteMailboxButton"));
        REQUIRE(deleteButton != nullptr);
        CHECK_FALSE(deleteButton->isEnabled());
        CHECK(deleteButton->toolTip().contains(QStringLiteral("child mailboxes")));
    }

    SECTION("server denies deletion")
    {
        auto value = mailbox();
        value.myRights.mayDelete = false;
        javelin::gui::mailboxes::MailboxPropertiesDialog dialog{
            QStringLiteral("Personal"), {}, value, false, false};
        auto* deleteButton = dialog.findChild<QPushButton*>(QStringLiteral("deleteMailboxButton"));
        REQUIRE(deleteButton != nullptr);
        CHECK_FALSE(deleteButton->isEnabled());
        CHECK(deleteButton->toolTip().contains(QStringLiteral("does not allow")));
    }
}
