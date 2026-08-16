#include "gui/messages/MessageConfirmationPresentation.h"
#include "gui/messages/MessageListModel.h"

#include <catch2/catch_test_macros.hpp>

#include <QDateTime>
#include <QLocale>
#include <QStandardItem>
#include <QStandardItemModel>

namespace
{
    [[nodiscard]] QStandardItem* messageItem(const QString& subject, const QString& sender,
                                             const QString& receivedAt,
                                             const qulonglong threadMessageCount = 1,
                                             const bool expanded = true)
    {
        auto* item = new QStandardItem;
        item->setData(subject, javelin::gui::messages::MessageListModel::SubjectRole);
        item->setData(sender, javelin::gui::messages::MessageListModel::SenderDisplayRole);
        item->setData(receivedAt, javelin::gui::messages::MessageListModel::ReceivedAtRole);
        item->setData(
            static_cast<int>(javelin::gui::messages::MessageListModel::RowKind::ThreadSummary),
            javelin::gui::messages::MessageListModel::RowKindRole);
        item->setData(threadMessageCount,
                      javelin::gui::messages::MessageListModel::GlobalThreadMessageCountRole);
        item->setData(expanded, javelin::gui::messages::MessageListModel::IsExpandedRole);
        return item;
    }
} // namespace

TEST_CASE("permanent message deletion identifies the selected message",
          "[gui][messages][confirmation]")
{
    QStandardItemModel model;
    const QString receivedAt = QStringLiteral("2026-08-15T09:30:00+12:00");
    model.appendRow(messageItem(QStringLiteral("Quarterly report"), QStringLiteral("Alice Example"),
                                receivedAt));
    const javelin::app::MessageSelection selection{
        javelin::app::SelectedEmail{.emailId = "email-1"}};

    const auto confirmation =
        javelin::gui::messages::permanentDeleteConfirmation(selection, {model.index(0, 0)}, {});
    const auto expectedTime = QLocale{}.toString(
        QDateTime::fromString(receivedAt, Qt::ISODate).toLocalTime(), QLocale::ShortFormat);

    CHECK(confirmation.prompt.contains(QStringLiteral("selected message")));
    CHECK(confirmation.details.contains(QStringLiteral("Quarterly report")));
    CHECK(confirmation.details.contains(QStringLiteral("Alice Example")));
    CHECK(confirmation.details.contains(expectedTime));
}

TEST_CASE("permanent conversation deletion identifies its message count",
          "[gui][messages][confirmation]")
{
    QStandardItemModel model;
    model.appendRow(messageItem(QStringLiteral("Project discussion"), QStringLiteral("Bob Example"),
                                QStringLiteral("2026-08-15T10:00:00+12:00"), 7, false));
    const javelin::app::MessageSelection selection{
        javelin::app::SelectedCollapsedThread{.threadId = "thread-1"}};

    const auto confirmation =
        javelin::gui::messages::permanentDeleteConfirmation(selection, {}, model.index(0, 0));

    CHECK(confirmation.prompt.contains(QStringLiteral("conversation")));
    CHECK(confirmation.details.contains(QStringLiteral("Project discussion")));
    CHECK(confirmation.details.contains(QStringLiteral("7 messages")));
}
