#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace javelin::gui::shell
{
    struct AccountActionAccount
    {
        std::string accountId;
        bool isReadOnly = false;
        bool isPrimary = false;
        bool hasMailCapability = false;
        bool hasSubmissionCapability = false;
    };

    struct AccountWorkspaceActionState
    {
        std::optional<std::string> preferredMailAccountId;
        std::optional<std::string> preferredSubmissionAccountId;
    };

    [[nodiscard]] AccountWorkspaceActionState
    accountWorkspaceActionState(std::span<const AccountActionAccount> accounts,
                                std::optional<std::string_view> preferredAccountId);
} // namespace javelin::gui::shell
