#include "gui/shell/MessageNavigationPolicy.h"

namespace javelin::gui::shell
{
    MessageNavigationPlan planMessageNavigation(const MessageNavigationPolicyInput& input)
    {
        if (input.route == nullptr || !input.activeAccountId.has_value() ||
            !input.activeMailboxId.has_value() ||
            *input.activeAccountId != input.route->accountId ||
            *input.activeMailboxId != input.route->mailboxId)
        {
            return {};
        }

        const auto selection = javelin::gui::messages::planMessageSelectionRestoration(
            input.rows, {
                            .threadId = input.route->threadId,
                            .emailId = input.route->emailId,
                            .selectedEmailIds = {},
                            .previousRow = std::nullopt,
                        });
        if (selection.currentRow.has_value())
        {
            return {
                .presentRoute = true,
                .currentRow = selection.currentRow,
                .completeRoute = true,
                .requestReveal = false,
            };
        }

        return {
            .presentRoute = true,
            .currentRow = std::nullopt,
            .completeRoute = false,
            .requestReveal = !input.mailboxRefreshInFlight && !input.revealAlreadyRequested,
        };
    }

    bool shouldCancelMessageNavigation(const javelin::app::OpenEmailRoute& route,
                                       const std::string_view selectedEmailId,
                                       const std::optional<std::string_view> selectedThreadId)
    {
        if (selectedEmailId == route.emailId)
            return false;
        return !route.threadId.has_value() || !selectedThreadId.has_value() ||
               *selectedThreadId != *route.threadId;
    }
} // namespace javelin::gui::shell
