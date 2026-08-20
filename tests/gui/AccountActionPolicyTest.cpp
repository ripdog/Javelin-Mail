#include "gui/shell/AccountActionPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
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
