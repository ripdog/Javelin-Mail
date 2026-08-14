#include "gui/messages/MessageActionSelection.h"
#include "gui/messages/MessageListModel.h"

#include <catch2/catch_test_macros.hpp>

#include <QStandardItem>
#include <QStandardItemModel>

#include <variant>

namespace
{
    [[nodiscard]] QStandardItem*
    messageItem(const QString& emailId, const QString& threadId,
                const javelin::gui::messages::MessageListModel::RowKind rowKind,
                const qulonglong threadMessageCount, const bool expanded, const bool unread)
    {
        auto* item = new QStandardItem;
        item->setData(emailId, javelin::gui::messages::MessageListModel::EmailIdRole);
        item->setData(threadId, javelin::gui::messages::MessageListModel::ThreadIdRole);
        item->setData(static_cast<int>(rowKind),
                      javelin::gui::messages::MessageListModel::RowKindRole);
        item->setData(threadMessageCount,
                      javelin::gui::messages::MessageListModel::ThreadMessageCountRole);
        item->setData(threadMessageCount > 1,
                      javelin::gui::messages::MessageListModel::CanExpandRole);
        item->setData(expanded, javelin::gui::messages::MessageListModel::IsExpandedRole);
        item->setData(unread, javelin::gui::messages::MessageListModel::IsUnreadRole);
        return item;
    }
} // namespace

TEST_CASE("message action selection preserves collapsed threads and filters unread rows",
          "[gui][messages][selection]")
{
    QStandardItemModel model;
    model.appendRow(messageItem(QStringLiteral("email-1"), QStringLiteral("thread-1"),
                                javelin::gui::messages::MessageListModel::RowKind::ThreadSummary, 3,
                                false, false));
    model.appendRow(messageItem(QStringLiteral("email-2"), QStringLiteral("thread-2"),
                                javelin::gui::messages::MessageListModel::RowKind::ThreadSummary, 1,
                                false, false));
    model.appendRow(messageItem(QStringLiteral("email-3"), QStringLiteral("thread-3"),
                                javelin::gui::messages::MessageListModel::RowKind::ThreadMember, 1,
                                true, true));

    const auto selection = javelin::gui::messages::messageSelectionForAction(
        {model.index(2, 0), model.index(1, 0), model.index(0, 0), model.index(1, 0)}, {}, true);

    REQUIRE(selection.size() == 2);
    const auto* collapsed = std::get_if<javelin::app::SelectedCollapsedThread>(&selection[0]);
    REQUIRE(collapsed != nullptr);
    CHECK(collapsed->threadId == "thread-1");

    const auto* singleKnownMemberThread =
        std::get_if<javelin::app::SelectedCollapsedThread>(&selection[1]);
    REQUIRE(singleKnownMemberThread != nullptr);
    CHECK(singleKnownMemberThread->threadId == "thread-2");
}

TEST_CASE("message action selection falls back to the current expanded summary",
          "[gui][messages][selection]")
{
    QStandardItemModel model;
    model.appendRow(messageItem(QStringLiteral("email-1"), QStringLiteral("thread-1"),
                                javelin::gui::messages::MessageListModel::RowKind::ThreadSummary, 4,
                                true, false));

    const auto selection = javelin::gui::messages::messageSelectionForAction({}, model.index(0, 0));

    REQUIRE(selection.size() == 1);
    const auto* email = std::get_if<javelin::app::SelectedEmail>(&selection.front());
    REQUIRE(email != nullptr);
    CHECK(email->emailId == "email-1");
}
