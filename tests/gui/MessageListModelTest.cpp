#include "gui/messages/MessageListModel.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/QueryService.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] javelin::jmap::cache::MessageListItem
    item(std::string emailId, std::string threadId, bool unread = false)
    {
        return {
            .emailId = std::move(emailId),
            .threadId = std::move(threadId),
            .subject = std::nullopt,
            .preview = std::nullopt,
            .receivedAt = {},
            .sentAt = std::nullopt,
            .threadMessageCount = 1,
            .hasAttachment = false,
            .isUnread = unread,
            .isFlagged = false,
            .isJunk = false,
            .from = std::nullopt,
            .mailboxNames = {},
        };
    }
} // namespace

TEST_CASE("message list model displays a placeholder for missing subjects",
          "[gui][messages][model]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    auto first = item("email-1", "thread-1");
    auto second = item("email-2", "thread-2");
    second.subject = std::string{};
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(first), std::move(second)});

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::SubjectRole)
              .toString() == QStringLiteral("<No Subject>"));
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::SubjectRole)
              .toString() == QStringLiteral("<No Subject>"));
}

TEST_CASE("message list model marks one cached row read without resetting its list",
          "[gui][messages][model]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {item("email-1", "thread-1", true), item("email-2", "thread-2", true)});

    REQUIRE(model.rowCount() == 2);
    CHECK(model.setEmailRead("email-1"));
    CHECK_FALSE(model.data(model.index(0), javelin::gui::messages::MessageListModel::IsUnreadRole)
                    .toBool());
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::IsUnreadRole)
              .toBool());
    CHECK_FALSE(model.setEmailRead("email-1"));
}

TEST_CASE("message list model appends an infinite-scroll tail without resetting existing rows",
          "[gui][messages][model][infinite-scroll]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {item("email-1", "thread-1"), item("email-2", "thread-2")});

    int resetCount = 0;
    int insertedCount = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, &model, [&] { ++resetCount; });
    QObject::connect(&model, &QAbstractItemModel::rowsInserted, &model,
                     [&](const QModelIndex&, const int first, const int last)
                     {
                         CHECK(first == 2);
                         CHECK(last == 3);
                         ++insertedCount;
                     });

    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {item("email-1", "thread-1"), item("email-2", "thread-2"),
                    item("email-3", "thread-3"), item("email-4", "thread-4")});

    CHECK(resetCount == 0);
    CHECK(insertedCount == 1);
    CHECK(model.rowCount() == 4);
    CHECK(model.data(model.index(3), javelin::gui::messages::MessageListModel::EmailIdRole)
              .toString() == QStringLiteral("email-4"));
}
