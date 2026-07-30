#pragma once

#include "app/MessageNavigationCoordinator.h"
#include "gui/messages/MessageSelectionRestoration.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace javelin::gui::shell
{
    struct MessageNavigationPolicyInput
    {
        const javelin::app::OpenEmailRoute* route = nullptr;
        std::optional<std::string_view> activeAccountId;
        std::optional<std::string_view> activeMailboxId;
        std::span<const javelin::gui::messages::MessageRowIdentity> rows;
        bool mailboxRefreshInFlight = false;
        bool revealAlreadyRequested = false;
    };

    struct MessageNavigationPlan
    {
        bool presentRoute = false;
        std::optional<std::size_t> currentRow;
        bool completeRoute = false;
        bool requestReveal = false;
    };

    [[nodiscard]] bool
    isStartedMessageNavigationRoute(const javelin::app::OpenEmailRoute* route,
                                    std::optional<std::uint64_t> startedRouteId,
                                    std::optional<std::string_view> activeAccountId,
                                    std::optional<std::string_view> activeMailboxId);
    [[nodiscard]] MessageNavigationPlan
    planMessageNavigation(const MessageNavigationPolicyInput& input);

    [[nodiscard]] bool
    shouldCancelMessageNavigation(const javelin::app::OpenEmailRoute& route,
                                  std::string_view selectedEmailId,
                                  std::optional<std::string_view> selectedThreadId);
} // namespace javelin::gui::shell
