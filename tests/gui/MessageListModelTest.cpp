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
            .mailboxThreadMessageCount = 1,
            .globalThreadMessageCount = 1,
            .hasAttachment = false,
            .isUnread = unread,
            .isFlagged = false,
            .isJunk = false,
            .from = std::nullopt,
            .mailboxNames = {},
            .tags = {},
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

TEST_CASE("message list model exposes tags already carried by message rows",
          "[gui][messages][model][tags]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    auto tagged = item("email-1", "thread-1");
    tagged.tags.push_back(javelin::jmap::cache::MessageListTag{
        .keyword = "work",
        .displayName = QStringLiteral("Work Items"),
        .color = QStringLiteral("#123456"),
    });
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(tagged)});

    REQUIRE(model.rowCount() == 1);
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::TagNamesRole)
              .toStringList() == QStringList{QStringLiteral("Work Items")});
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::TagColorsRole)
              .toStringList() == QStringList{QStringLiteral("#123456")});
}

TEST_CASE("message list model normalizes tooltips and prefers cached body previews",
          "[gui][messages][model][tooltip]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    auto serverPreview = item("email-1", "thread-1");
    serverPreview.preview = "\n\n      Server   preview\n   text   ";
    auto bodyPreview = item("email-2", "thread-2");
    bodyPreview.preview = "Server preview should not win";
    bodyPreview.bodyPreview = "\n\t  Plaintext    body\npreview  ";
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(serverPreview), std::move(bodyPreview)});

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0), Qt::ToolTipRole).toString() ==
          QStringLiteral("Server preview text"));
    CHECK(model.data(model.index(1), Qt::ToolTipRole).toString() ==
          QStringLiteral("Plaintext body preview"));
}

TEST_CASE("message list model exposes painted message state to accessibility",
          "[gui][messages][model][accessibility]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    auto accessible = item("email-1", "thread-1", true);
    accessible.subject = "Quarterly update";
    accessible.preview = "The preview text";
    accessible.receivedAt = "2026-08-10T08:15:00+12:00";
    accessible.mailboxThreadMessageCount = 2;
    accessible.globalThreadMessageCount = 2;
    accessible.hasAttachment = true;
    accessible.isFlagged = true;
    accessible.from =
        javelin::jmap::domain::EmailAddress{.name = "Alice", .email = "alice@example.com"};
    accessible.mailboxNames = {"Inbox", "Projects"};
    accessible.tags.push_back(javelin::jmap::cache::MessageListTag{
        .keyword = "work",
        .displayName = QStringLiteral("Work"),
        .color = QStringLiteral("#123456"),
    });
    model.setItems(std::optional<std::string>{"account-1"}, std::nullopt, {std::move(accessible)});

    const auto index = model.index(0);
    const auto text = model.data(index, Qt::AccessibleTextRole).toString();
    CHECK(text.contains(QStringLiteral("Alice")));
    CHECK(text.contains(QStringLiteral("Quarterly update")));
    CHECK(text.contains(QStringLiteral("Unread")));
    CHECK(text.contains(QStringLiteral("Starred")));
    CHECK(text.contains(QStringLiteral("Has attachment")));
    CHECK(text.contains(QStringLiteral("2 messages in this mailbox")));
    CHECK(text.contains(QStringLiteral("Work")));
    CHECK(text.contains(QStringLiteral("Inbox, Projects")));
    CHECK(model.data(index, Qt::AccessibleDescriptionRole).toString() ==
          QStringLiteral("Preview: The preview text"));
}

TEST_CASE("message list model expands a known conversation without an exact mailbox count",
          "[gui][messages][model][accessibility][thread-coverage]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    auto conversation = item("email-1", "thread-1");
    conversation.mailboxThreadMessageCount.reset();
    conversation.globalThreadMessageCount = 3;
    model.setItems(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                   {std::move(conversation)});

    const auto index = model.index(0);
    CHECK(model.data(index, javelin::gui::messages::MessageListModel::CanExpandRole).toBool());
    CHECK_FALSE(model.data(index, javelin::gui::messages::MessageListModel::ThreadMessageCountRole)
                    .isValid());
    CHECK(model.data(index, javelin::gui::messages::MessageListModel::GlobalThreadMessageCountRole)
              .toULongLong() == 3);
    const auto accessibleText = model.data(index, Qt::AccessibleTextRole).toString();
    CHECK(accessibleText.contains(QStringLiteral("Conversation")));
    CHECK_FALSE(accessibleText.contains(QStringLiteral("3 messages")));
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
