#include "gui/shell/MessageNavigationPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace javelin::gui::shell;

namespace
{
    const javelin::app::OpenEmailRoute route{
        .id = 7,
        .accountId = "account",
        .mailboxId = "inbox",
        .threadId = "thread",
        .emailId = "email",
    };
}

TEST_CASE("message navigation ignores routes for another active mailbox")
{
    const auto plan = planMessageNavigation({
        .route = &route,
        .activeAccountId = std::string_view{"account"},
        .activeMailboxId = std::string_view{"archive"},
        .rows = {},
        .mailboxRefreshInFlight = false,
        .revealAlreadyRequested = false,
    });

    CHECK_FALSE(plan.presentRoute);
    CHECK_FALSE(plan.currentRow.has_value());
    CHECK_FALSE(plan.completeRoute);
    CHECK_FALSE(plan.requestReveal);
}

TEST_CASE("message navigation is inactive until the GUI begins the route")
{
    CHECK_FALSE(isStartedMessageNavigationRoute(&route, std::nullopt, std::string_view{"account"},
                                                std::string_view{"inbox"}));
    CHECK_FALSE(isStartedMessageNavigationRoute(&route, route.id - 1, std::string_view{"account"},
                                                std::string_view{"inbox"}));
    CHECK(isStartedMessageNavigationRoute(&route, route.id, std::string_view{"account"},
                                          std::string_view{"inbox"}));
}

TEST_CASE("message navigation completes when the requested email is visible")
{
    const std::array rows{
        javelin::gui::messages::MessageRowIdentity{.threadId = "other", .emailId = "other"},
        javelin::gui::messages::MessageRowIdentity{.threadId = "thread", .emailId = "email"},
    };
    const auto plan = planMessageNavigation({
        .route = &route,
        .activeAccountId = std::string_view{"account"},
        .activeMailboxId = std::string_view{"inbox"},
        .rows = rows,
        .mailboxRefreshInFlight = false,
        .revealAlreadyRequested = false,
    });

    CHECK(plan.presentRoute);
    CHECK(plan.currentRow == std::optional<std::size_t>{1});
    CHECK(plan.completeRoute);
    CHECK_FALSE(plan.requestReveal);
}

TEST_CASE("message navigation reveals a missing email only once")
{
    const auto first = planMessageNavigation({
        .route = &route,
        .activeAccountId = std::string_view{"account"},
        .activeMailboxId = std::string_view{"inbox"},
        .rows = {},
        .mailboxRefreshInFlight = false,
        .revealAlreadyRequested = false,
    });
    const auto repeated = planMessageNavigation({
        .route = &route,
        .activeAccountId = std::string_view{"account"},
        .activeMailboxId = std::string_view{"inbox"},
        .rows = {},
        .mailboxRefreshInFlight = false,
        .revealAlreadyRequested = true,
    });

    CHECK(first.presentRoute);
    CHECK(first.requestReveal);
    CHECK(repeated.presentRoute);
    CHECK_FALSE(repeated.requestReveal);
}

TEST_CASE("message navigation remains pending on a same-thread placeholder")
{
    const std::array rows{
        javelin::gui::messages::MessageRowIdentity{
            .threadId = "thread",
            .emailId = "older-email",
        },
    };
    const auto plan = planMessageNavigation({
        .route = &route,
        .activeAccountId = std::string_view{"account"},
        .activeMailboxId = std::string_view{"inbox"},
        .rows = rows,
        .mailboxRefreshInFlight = false,
        .revealAlreadyRequested = false,
    });

    CHECK(plan.presentRoute);
    CHECK(plan.currentRow == std::optional<std::size_t>{0});
    CHECK_FALSE(plan.completeRoute);
    CHECK(plan.requestReveal);
}

TEST_CASE("message navigation waits for an in-flight mailbox refresh")
{
    const auto plan = planMessageNavigation({
        .route = &route,
        .activeAccountId = std::string_view{"account"},
        .activeMailboxId = std::string_view{"inbox"},
        .rows = {},
        .mailboxRefreshInFlight = true,
        .revealAlreadyRequested = false,
    });

    CHECK(plan.presentRoute);
    CHECK_FALSE(plan.completeRoute);
    CHECK_FALSE(plan.requestReveal);
}

TEST_CASE("message navigation survives selecting another email in the requested thread")
{
    CHECK_FALSE(shouldCancelMessageNavigation(route, "another-email",
                                              std::optional<std::string_view>{"thread"}));
    CHECK(shouldCancelMessageNavigation(route, "another-email",
                                        std::optional<std::string_view>{"another-thread"}));
    CHECK_FALSE(shouldCancelMessageNavigation(route, "email", std::nullopt));
}
