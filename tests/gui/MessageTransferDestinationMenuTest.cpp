#include "gui/shell/MessageTransferDestinationMenu.h"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>

#include <optional>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] javelin::gui::shell::MessageTransferDestinationRow
    row(std::string accountId, std::string mailboxId, std::string name, QString path,
        QString accountLabel, const std::size_t depth = 0, const bool separatorBefore = false)
    {
        return {
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .mailboxName = std::move(name),
            .role = std::nullopt,
            .mailboxPath = std::move(path),
            .accountLabel = std::move(accountLabel),
            .depth = depth,
            .separatorBefore = separatorBefore,
        };
    }

    [[nodiscard]] QAction* destinationAction(QMenu& menu, const QString& mailboxId)
    {
        for (auto* action : menu.actions())
        {
            if (action->property("javelinDestinationMailboxId").toString() == mailboxId)
                return action;
        }
        return nullptr;
    }
} // namespace

TEST_CASE("transfer destination menu keeps hierarchy until search text is entered",
          "[gui][mail-transfer][destination][menu]")
{
    javelin::gui::shell::MessageTransferDestinationPresentation presentation{
        .currentAccountLabel = QStringLiteral("Personal"),
        .currentAccountRows =
            {
                row("a", "inbox", "Inbox", QStringLiteral("Inbox"), QStringLiteral("Personal")),
                row("a", "child", "Child", QStringLiteral("Parent / Child"),
                    QStringLiteral("Personal"), 1),
            },
        .otherAccounts = {{.accountId = "b",
                           .label = QStringLiteral("Work"),
                           .rows = {row("b", "archive", "Archive", QStringLiteral("Archive"),
                                        QStringLiteral("Work"))}}},
    };

    QMenu menu;
    std::optional<std::string> triggeredMailbox;
    REQUIRE(javelin::gui::shell::populateMessageTransferDestinationMenu(
        menu, presentation, [&triggeredMailbox](const auto& destination)
        { triggeredMailbox = destination.mailboxId; }));

    auto* search = menu.findChild<QLineEdit*>(QStringLiteral("messageTransferDestinationSearch"));
    REQUIRE(search != nullptr);
    CHECK(destinationAction(menu, QStringLiteral("inbox")) != nullptr);
    CHECK(destinationAction(menu, QStringLiteral("child")) != nullptr);
    REQUIRE(menu.findChildren<QMenu*>().size() == 1);

    search->setText(QStringLiteral("work arch"));
    auto* archive = destinationAction(menu, QStringLiteral("archive"));
    REQUIRE(archive != nullptr);
    CHECK(archive->text() == QStringLiteral("Archive — Work"));
    CHECK(destinationAction(menu, QStringLiteral("inbox")) == nullptr);
    CHECK(menu.findChildren<QMenu*>().empty());
    CHECK(menu.activeAction() == archive);

    QKeyEvent enter{QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &enter);
    REQUIRE(triggeredMailbox.has_value());
    CHECK(*triggeredMailbox == "archive");
}

TEST_CASE("printable transfer-menu typing focuses search and arrows select fuzzy results",
          "[gui][mail-transfer][destination][menu][keyboard]")
{
    javelin::gui::shell::MessageTransferDestinationPresentation presentation{
        .currentAccountLabel = QStringLiteral("Personal"),
        .currentAccountRows =
            {
                row("a", "projects", "Projects", QStringLiteral("Projects"),
                    QStringLiteral("Personal")),
                row("a", "people", "People", QStringLiteral("People"), QStringLiteral("Personal")),
            },
        .otherAccounts = {},
    };

    QMenu menu;
    REQUIRE(javelin::gui::shell::populateMessageTransferDestinationMenu(menu, presentation,
                                                                        [](const auto&) {}));
    auto* search = menu.findChild<QLineEdit*>(QStringLiteral("messageTransferDestinationSearch"));
    REQUIRE(search != nullptr);

    QKeyEvent typeP{QEvent::KeyPress, Qt::Key_P, Qt::NoModifier, QStringLiteral("p")};
    QCoreApplication::sendEvent(&menu, &typeP);
    CHECK(search->text() == QStringLiteral("p"));

    auto* first = menu.activeAction();
    REQUIRE(first != nullptr);
    QKeyEvent down{QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &down);
    auto* second = menu.activeAction();
    REQUIRE(second != nullptr);
    CHECK(second != first);

    QKeyEvent up{QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier};
    QCoreApplication::sendEvent(search, &up);
    CHECK(menu.activeAction() == first);
}
