#include "gui/shell/AccountActionPolicy.h"

#include <algorithm>
#include <ranges>

namespace javelin::gui::shell
{
    AccountWorkspaceActionState
    accountWorkspaceActionState(const std::span<const AccountActionAccount> accounts,
                                const std::optional<std::string_view> preferredAccountId)
    {
        const auto mailCapable = [](const AccountActionAccount& account)
        { return account.hasMailCapability; };
        const auto submissionCapable = [](const AccountActionAccount& account)
        {
            return account.hasMailCapability && account.hasSubmissionCapability &&
                   !account.isReadOnly;
        };
        const auto preferred =
            [&accounts, preferredAccountId](const auto predicate) -> std::optional<std::string>
        {
            if (!preferredAccountId.has_value())
                return std::nullopt;
            const auto found =
                std::ranges::find(accounts, *preferredAccountId, &AccountActionAccount::accountId);
            return found != accounts.end() && predicate(*found)
                       ? std::optional<std::string>{found->accountId}
                       : std::nullopt;
        };
        const auto primary = [&accounts](const auto predicate) -> std::optional<std::string>
        {
            const auto found =
                std::ranges::find_if(accounts, [&predicate](const AccountActionAccount& account)
                                     { return account.isPrimary && predicate(account); });
            return found == accounts.end() ? std::nullopt
                                           : std::optional<std::string>{found->accountId};
        };
        const auto first = [&accounts](const auto predicate) -> std::optional<std::string>
        {
            const auto found = std::ranges::find_if(accounts, predicate);
            return found == accounts.end() ? std::nullopt
                                           : std::optional<std::string>{found->accountId};
        };

        auto mail = preferred(mailCapable);
        if (!mail.has_value())
            mail = primary(mailCapable);
        if (!mail.has_value())
            mail = first(mailCapable);

        auto submission = preferred(submissionCapable);
        if (!submission.has_value() && mail.has_value())
        {
            const auto mailAccount =
                std::ranges::find(accounts, *mail, &AccountActionAccount::accountId);
            if (mailAccount != accounts.end() && submissionCapable(*mailAccount))
                submission = mailAccount->accountId;
        }
        if (!submission.has_value())
            submission = primary(submissionCapable);
        if (!submission.has_value())
            submission = first(submissionCapable);

        return {.preferredMailAccountId = std::move(mail),
                .preferredSubmissionAccountId = std::move(submission)};
    }
} // namespace javelin::gui::shell
