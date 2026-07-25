#include "gui/shell/MessageListTabPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

namespace
{
    using Collection = javelin::gui::shell::MessageListTabCollection;
    using Identity = javelin::gui::shell::MessageListTabIdentity;

    const std::vector<std::optional<Identity>> tabs{
        Identity{
            .collection = Collection::Mailbox, .accountId = "account-a", .collectionKey = "inbox"},
        Identity{.collection = Collection::Mailbox,
                 .accountId = "account-a",
                 .collectionKey = "archive"},
        std::nullopt,
        Identity{.collection = Collection::Search,
                 .accountId = "account-a",
                 .collectionKey = "from:alice"},
        Identity{.collection = Collection::Search,
                 .accountId = "account-b",
                 .collectionKey = "from:alice"},
    };
} // namespace

TEST_CASE("message list tab reuse matches collection account and key", "[gui][tabs][lifecycle]")
{
    using javelin::gui::shell::findReusableMessageListTab;

    CHECK(findReusableMessageListTab(tabs, {.collection = Collection::Mailbox,
                                            .accountId = "account-a",
                                            .collectionKey = "archive"}) ==
          std::optional<std::size_t>{1});
    CHECK(findReusableMessageListTab(tabs, {.collection = Collection::Search,
                                            .accountId = "account-a",
                                            .collectionKey = "from:alice"}) ==
          std::optional<std::size_t>{3});
    CHECK_FALSE(findReusableMessageListTab(tabs, {.collection = Collection::Search,
                                                  .accountId = "account-a",
                                                  .collectionKey = "from:bob"})
                    .has_value());
}

TEST_CASE("message list tab reuse can exclude the home tab", "[gui][tabs][lifecycle]")
{
    using javelin::gui::shell::findReusableMessageListTab;

    CHECK_FALSE(
        findReusableMessageListTab(
            tabs,
            {.collection = Collection::Mailbox, .accountId = "account-a", .collectionKey = "inbox"},
            1)
            .has_value());
}

TEST_CASE("message list stale policy excludes the refreshed mailbox", "[gui][tabs][lifecycle]")
{
    const auto indexes = javelin::gui::shell::messageListTabsToMarkStale(tabs, "account-a",
                                                                         std::string_view{"inbox"});

    CHECK(indexes == std::vector<std::size_t>{1, 3});
}

TEST_CASE("message list stale policy can target all account tabs", "[gui][tabs][lifecycle]")
{
    const auto indexes = javelin::gui::shell::messageListTabsToMarkStale(tabs, "account-a");

    CHECK(indexes == std::vector<std::size_t>{0, 1, 3});
}

TEST_CASE("message list stale policy can target searches only", "[gui][tabs][lifecycle]")
{
    const auto indexes =
        javelin::gui::shell::messageListTabsToMarkStale(tabs, "account-a", std::nullopt, true);

    CHECK(indexes == std::vector<std::size_t>{3});
}
