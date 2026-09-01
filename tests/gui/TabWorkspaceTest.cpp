#include "gui/shell/TabWorkspace.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] javelin::gui::shell::TabState mailboxTab()
    {
        return {.content = javelin::gui::shell::MailboxTabState{}};
    }

    [[nodiscard]] javelin::gui::shell::TabState searchTab()
    {
        return {.content = javelin::gui::shell::SearchTabState{}};
    }

    [[nodiscard]] javelin::gui::shell::TabState composeTab(std::string accountId = "account-a")
    {
        return {.content = javelin::gui::shell::ComposeTabState{
                    .accountId = std::move(accountId),
                    .composeSessionId = "compose-a",
                    .title = QStringLiteral("Compose"),
                    .selection = {},
                }};
    }

    [[nodiscard]] javelin::gui::shell::TabState contactsTab()
    {
        return {.content = javelin::gui::shell::ContactsTabState{
                    .title = QStringLiteral("Contacts"),
                    .selection = {},
                }};
    }

    [[nodiscard]] javelin::gui::shell::TabState calendarTab()
    {
        return {.content = javelin::gui::shell::CalendarTabState{
                    .title = QStringLiteral("Calendar"),
                    .selection = {},
                }};
    }
} // namespace

TEST_CASE("tab workspace resolves active tabs only for valid indexes", "[gui][tabs]")
{
    std::vector<javelin::gui::shell::TabState> tabs;
    tabs.push_back(mailboxTab());
    tabs.push_back(composeTab());

    CHECK(javelin::gui::shell::activeWorkspaceTab(tabs, std::nullopt) == nullptr);
    CHECK(javelin::gui::shell::activeWorkspaceTab(tabs, -1) == nullptr);
    CHECK(javelin::gui::shell::activeWorkspaceTab(tabs, 2) == nullptr);
    REQUIRE(javelin::gui::shell::activeWorkspaceTab(tabs, 1) != nullptr);
    CHECK(javelin::gui::shell::tabKind(*javelin::gui::shell::activeWorkspaceTab(tabs, 1)) ==
          javelin::gui::shell::TabKind::Compose);
}

TEST_CASE("tab workspace preserves the non-closeable mail home tab", "[gui][tabs]")
{
    const auto home = mailboxTab();
    const auto compose = composeTab();
    const auto contacts = contactsTab();

    CHECK_FALSE(javelin::gui::shell::tabCanClose(home, 0));
    CHECK(javelin::gui::shell::tabCanClose(home, 1));
    CHECK(javelin::gui::shell::tabCanClose(compose, 0));
    CHECK(javelin::gui::shell::tabCanClose(contacts, 0));
}

TEST_CASE("tab workspace close policy preserves the active tab by identity", "[gui][tabs]")
{
    using javelin::gui::shell::activeTabIndexAfterClose;

    CHECK(activeTabIndexAfterClose(1, 0, 0) == std::nullopt);
    CHECK(activeTabIndexAfterClose(4, 0, 2) == std::optional<int>{0});
    CHECK(activeTabIndexAfterClose(4, 2, 2) == std::optional<int>{1});
    CHECK(activeTabIndexAfterClose(4, 3, 1) == std::optional<int>{2});
    CHECK(activeTabIndexAfterClose(4, 3, 2) == std::optional<int>{2});
    // Editing a search can append its replacement, then remove the old search before it.
    CHECK(activeTabIndexAfterClose(5, 4, 2) == std::optional<int>{3});
    CHECK(activeTabIndexAfterClose(4, std::nullopt, 3) == std::optional<int>{2});
    CHECK(activeTabIndexAfterClose(4, 2, 8) == std::optional<int>{2});
}

TEST_CASE("tab workspace tracks the active tab across tab moves", "[gui][tabs]")
{
    using javelin::gui::shell::activeTabIndexAfterMove;

    CHECK(activeTabIndexAfterMove(2, 2, 4) == std::optional<int>{4});
    CHECK(activeTabIndexAfterMove(3, 1, 4) == std::optional<int>{2});
    CHECK(activeTabIndexAfterMove(2, 4, 1) == std::optional<int>{3});
    CHECK(activeTabIndexAfterMove(0, 2, 4) == std::optional<int>{0});
    CHECK(activeTabIndexAfterMove(std::nullopt, 2, 4) == std::nullopt);
    CHECK(activeTabIndexAfterMove(2, -1, 4) == std::optional<int>{2});
    CHECK(activeTabIndexAfterMove(2, 2, 2) == std::optional<int>{2});
}

TEST_CASE("mail and search tabs own expansion intent independently of reusable presentation",
          "[gui][tabs][thread-expansion]")
{
    auto mailboxA = mailboxTab();
    auto mailboxB = mailboxTab();
    auto search = searchTab();
    auto compose = composeTab();
    auto contacts = contactsTab();
    auto calendar = calendarTab();

    auto* mailboxAExpanded = javelin::gui::shell::tabExpandedThreadIds(mailboxA);
    auto* mailboxBExpanded = javelin::gui::shell::tabExpandedThreadIds(mailboxB);
    auto* searchExpanded = javelin::gui::shell::tabExpandedThreadIds(search);
    REQUIRE(mailboxAExpanded != nullptr);
    REQUIRE(mailboxBExpanded != nullptr);
    REQUIRE(searchExpanded != nullptr);
    *mailboxAExpanded = {"thread-a"};
    *mailboxBExpanded = {"thread-b"};
    *searchExpanded = {"thread-search"};

    CHECK(*javelin::gui::shell::tabExpandedThreadIds(std::as_const(mailboxA)) ==
          std::vector<std::string>{"thread-a"});
    CHECK(*javelin::gui::shell::tabExpandedThreadIds(std::as_const(mailboxB)) ==
          std::vector<std::string>{"thread-b"});
    CHECK(*javelin::gui::shell::tabExpandedThreadIds(std::as_const(search)) ==
          std::vector<std::string>{"thread-search"});
    CHECK(javelin::gui::shell::tabExpandedThreadIds(compose) == nullptr);
    CHECK(javelin::gui::shell::tabExpandedThreadIds(contacts) == nullptr);
    CHECK(javelin::gui::shell::tabExpandedThreadIds(calendar) == nullptr);
}

TEST_CASE("tab workspace exposes one selection state for every tab kind", "[gui][tabs]")
{
    auto tab = composeTab();
    auto& selection = javelin::gui::shell::tabSelection(tab);
    selection.emailId = "email-a";
    selection.selectedEmailIds = {"email-a", "email-b"};

    const auto& restored = javelin::gui::shell::tabSelection(std::as_const(tab));
    CHECK(restored.emailId == std::optional<std::string>{"email-a"});
    CHECK(restored.selectedEmailIds == std::vector<std::string>{"email-a", "email-b"});
}
