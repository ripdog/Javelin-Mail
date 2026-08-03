#include "gui/messages/MessageListModel.h"
#include "jmap/cache/Database.h"
#include "jmap/cache/QueryService.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <optional>
#include <string>
#include <vector>

TEST_CASE("message list model displays a placeholder for missing subjects",
          "[gui][messages][model]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    model.setPage(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                  {
                      {
                          .emailId = "email-1",
                          .threadId = "thread-1",
                          .subject = std::nullopt,
                          .preview = std::nullopt,
                          .receivedAt = {},
                          .sentAt = std::nullopt,
                          .threadMessageCount = 1,
                          .hasAttachment = false,
                          .isUnread = false,
                          .isFlagged = false,
                          .from = std::nullopt,
                          .mailboxNames = {},
                      },
                      {
                          .emailId = "email-2",
                          .threadId = "thread-2",
                          .subject = std::string{},
                          .preview = std::nullopt,
                          .receivedAt = {},
                          .sentAt = std::nullopt,
                          .threadMessageCount = 1,
                          .hasAttachment = false,
                          .isUnread = false,
                          .isFlagged = false,
                          .from = std::nullopt,
                          .mailboxNames = {},
                      },
                  });

    REQUIRE(model.rowCount() == 2);
    CHECK(model.data(model.index(0), javelin::gui::messages::MessageListModel::SubjectRole)
              .toString() == QStringLiteral("<No Subject>"));
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::SubjectRole)
              .toString() == QStringLiteral("<No Subject>"));
}

TEST_CASE("message list model marks one cached row read without resetting its page",
          "[gui][messages][model]")
{
    javelin::jmap::cache::DatabaseConnection connection;
    javelin::jmap::cache::QueryService queryService{connection};
    javelin::gui::messages::MessageListModel model{queryService};

    model.setPage(std::optional<std::string>{"account-1"}, std::optional<std::string>{"mailbox-1"},
                  {
                      {
                          .emailId = "email-1",
                          .threadId = "thread-1",
                          .subject = std::nullopt,
                          .preview = std::nullopt,
                          .receivedAt = {},
                          .sentAt = std::nullopt,
                          .threadMessageCount = 1,
                          .hasAttachment = false,
                          .isUnread = true,
                          .isFlagged = false,
                          .from = std::nullopt,
                          .mailboxNames = {},
                      },
                      {
                          .emailId = "email-2",
                          .threadId = "thread-2",
                          .subject = std::nullopt,
                          .preview = std::nullopt,
                          .receivedAt = {},
                          .sentAt = std::nullopt,
                          .threadMessageCount = 1,
                          .hasAttachment = false,
                          .isUnread = true,
                          .isFlagged = false,
                          .from = std::nullopt,
                          .mailboxNames = {},
                      },
                  });

    REQUIRE(model.rowCount() == 2);
    CHECK(model.setEmailRead("email-1"));
    CHECK_FALSE(model.data(model.index(0), javelin::gui::messages::MessageListModel::IsUnreadRole)
                    .toBool());
    CHECK(model.data(model.index(1), javelin::gui::messages::MessageListModel::IsUnreadRole)
              .toBool());
    CHECK_FALSE(model.setEmailRead("email-1"));
}
