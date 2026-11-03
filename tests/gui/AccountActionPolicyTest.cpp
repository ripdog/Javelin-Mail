#include "gui/shell/AccountActionPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

TEST_CASE("account action policy prefers the active capable mail account",
          "[gui][shell][actions][accounts]")
{
    const std::vector<javelin::gui::shell::AccountActionAccount> accounts{
        {.accountId = "primary",
         .isReadOnly = false,
         .isPrimary = true,
         .hasMailCapability = true,
         .hasSubmissionCapability = true},
        {.accountId = "active",
         .isReadOnly = false,
         .isPrimary = false,
         .hasMailCapability = true,
         .hasSubmissionCapability = true},
    };

    const auto state = javelin::gui::shell::accountWorkspaceActionState(
        accounts, std::optional<std::string_view>{"active"});

    CHECK(state.preferredMailAccountId == std::optional<std::string>{"active"});
    CHECK(state.preferredSubmissionAccountId == std::optional<std::string>{"active"});
}

TEST_CASE("account action policy falls back from a non-mail workspace account",
          "[gui][shell][actions][accounts]")
{
    const std::vector<javelin::gui::shell::AccountActionAccount> accounts{
        {.accountId = "contacts-only",
         .isReadOnly = false,
         .isPrimary = false,
         .hasMailCapability = false,
         .hasSubmissionCapability = false},
        {.accountId = "primary-mail",
         .isReadOnly = false,
         .isPrimary = true,
         .hasMailCapability = true,
         .hasSubmissionCapability = false},
        {.accountId = "sender",
         .isReadOnly = false,
         .isPrimary = false,
         .hasMailCapability = true,
         .hasSubmissionCapability = true},
    };

    const auto state = javelin::gui::shell::accountWorkspaceActionState(
        accounts, std::optional<std::string_view>{"contacts-only"});

    CHECK(state.preferredMailAccountId == std::optional<std::string>{"primary-mail"});
    CHECK(state.preferredSubmissionAccountId == std::optional<std::string>{"sender"});
}

TEST_CASE("account action policy never selects a read-only submission account",
          "[gui][shell][actions][accounts]")
{
    const std::vector<javelin::gui::shell::AccountActionAccount> accounts{
        {.accountId = "readonly",
         .isReadOnly = true,
         .isPrimary = true,
         .hasMailCapability = true,
         .hasSubmissionCapability = true},
        {.accountId = "writable",
         .isReadOnly = false,
         .isPrimary = false,
         .hasMailCapability = true,
         .hasSubmissionCapability = true},
    };

    const auto state = javelin::gui::shell::accountWorkspaceActionState(
        accounts, std::optional<std::string_view>{"readonly"});

    CHECK(state.preferredMailAccountId == std::optional<std::string>{"readonly"});
    CHECK(state.preferredSubmissionAccountId == std::optional<std::string>{"writable"});
}

TEST_CASE("account action policy reports unavailable mail actions without mail capability",
          "[gui][shell][actions][accounts]")
{
    const std::vector<javelin::gui::shell::AccountActionAccount> accounts{
        {.accountId = "calendar-only",
         .isReadOnly = false,
         .isPrimary = true,
         .hasMailCapability = false,
         .hasSubmissionCapability = false},
    };

    const auto state = javelin::gui::shell::accountWorkspaceActionState(accounts, std::nullopt);

    CHECK_FALSE(state.preferredMailAccountId.has_value());
    CHECK_FALSE(state.preferredSubmissionAccountId.has_value());
}

TEST_CASE("removed account tabs are listed from the last tab to the first",
          "[gui][shell][accounts][tabs]")
{
    const std::vector<javelin::gui::shell::AccountActionAccount> accounts{
        {.accountId = "kept", .hasMailCapability = true},
    };
    const std::vector<std::optional<std::string>> tabAccounts{
        std::optional<std::string>{"removed"}, // home mailbox
        std::optional<std::string>{"kept"},
        std::nullopt, // contacts or calendar
        std::optional<std::string>{"removed"},
    };

    const auto tabs = javelin::gui::shell::tabIndexesOfRemovedAccounts(tabAccounts, accounts);

    CHECK(tabs == std::vector<std::size_t>{3, 0});
}

TEST_CASE("every account tab is removed when no account remains", "[gui][shell][accounts][tabs]")
{
    const std::vector<std::optional<std::string>> tabAccounts{
        std::optional<std::string>{"removed"},
        std::nullopt,
    };

    CHECK(javelin::gui::shell::tabIndexesOfRemovedAccounts(tabAccounts, {}) ==
          std::vector<std::size_t>{0});
}

TEST_CASE("no tabs are removed while their accounts remain", "[gui][shell][accounts][tabs]")
{
    const std::vector<javelin::gui::shell::AccountActionAccount> accounts{
        {.accountId = "a", .hasMailCapability = true},
        {.accountId = "b", .hasMailCapability = true},
    };
    const std::vector<std::optional<std::string>> tabAccounts{
        std::optional<std::string>{"a"},
        std::optional<std::string>{"b"},
    };

    CHECK(javelin::gui::shell::tabIndexesOfRemovedAccounts(tabAccounts, accounts).empty());
}
