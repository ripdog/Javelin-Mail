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

TEST_CASE("tab workspace close policy matches the existing previous-tab selection", "[gui][tabs]")
{
    using javelin::gui::shell::activeTabIndexAfterClose;

    CHECK(activeTabIndexAfterClose(1, 0, 0) == std::nullopt);
    CHECK(activeTabIndexAfterClose(4, 0, 2) == std::optional<int>{0});
    CHECK(activeTabIndexAfterClose(4, 2, 2) == std::optional<int>{1});
    CHECK(activeTabIndexAfterClose(4, 3, 1) == std::optional<int>{0});
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
