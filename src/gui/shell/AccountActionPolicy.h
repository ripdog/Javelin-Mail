#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    /// Returns the indexes, highest first, of tabs bound to an account that is no longer present.
    /// Tabs without an account, such as contacts and calendar, are never selected.
    [[nodiscard]] std::vector<std::size_t>
    tabIndexesOfRemovedAccounts(std::span<const std::optional<std::string>> tabAccountIds,
                                std::span<const AccountActionAccount> accounts);
} // namespace javelin::gui::shell
