#include "gui/mailboxes/MailboxSelection.h"

#include "gui/mailboxes/MailboxTreeModel.h"

#include <catch2/catch_test_macros.hpp>

#include <QStandardItem>
#include <QStandardItemModel>

using namespace javelin::gui::mailboxes;

TEST_CASE("mailbox selection finds account roots and nested mailboxes")
{
    QStandardItemModel model;
    auto* account = new QStandardItem(QStringLiteral("Account"));
    account->setData(QStringLiteral("account"), MailboxTreeModel::AccountIdRole);
    account->setData(QString{}, MailboxTreeModel::MailboxIdRole);

    auto* inbox = new QStandardItem(QStringLiteral("Inbox"));
    inbox->setData(QStringLiteral("account"), MailboxTreeModel::AccountIdRole);
    inbox->setData(QStringLiteral("inbox"), MailboxTreeModel::MailboxIdRole);
    account->appendRow(inbox);
    model.appendRow(account);

    const auto accountIndex =
        findMailboxIndexForSelection(model, QStringLiteral("account"), std::nullopt);
    const auto inboxIndex = findMailboxIndexForSelection(
        model, QStringLiteral("account"), std::optional<QString>{QStringLiteral("inbox")});

    REQUIRE(accountIndex.isValid());
    CHECK(accountIndex.data(MailboxTreeModel::MailboxIdRole).toString().isEmpty());
    REQUIRE(inboxIndex.isValid());
    CHECK(inboxIndex.data(MailboxTreeModel::MailboxIdRole).toString() == QStringLiteral("inbox"));
}

TEST_CASE("mailbox selection does not cross account boundaries")
{
    QStandardItemModel model;
    auto* inbox = new QStandardItem(QStringLiteral("Inbox"));
    inbox->setData(QStringLiteral("first-account"), MailboxTreeModel::AccountIdRole);
    inbox->setData(QStringLiteral("inbox"), MailboxTreeModel::MailboxIdRole);
    model.appendRow(inbox);

    CHECK_FALSE(findMailboxIndexForSelection(model, QStringLiteral("second-account"),
                                             std::optional<QString>{QStringLiteral("inbox")})
                    .isValid());
}
