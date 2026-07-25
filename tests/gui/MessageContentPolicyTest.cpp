#include "gui/shell/MessageContentPolicy.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using namespace javelin::gui::shell;

TEST_CASE("message content applies to the sole selected message")
{
    const std::array selected{std::string{"email"}};
    CHECK(ownsMessageContentResult({
        .requestAccountId = "account",
        .requestEmailId = "email",
        .activeAccountId = std::string_view{"account"},
        .selectedEmailIds = selected,
        .routeAccountId = std::nullopt,
        .routeEmailId = std::nullopt,
    }));
}

TEST_CASE("message content does not replace a changed or multiple selection")
{
    const std::array changed{std::string{"other"}};
    const std::array multiple{std::string{"email"}, std::string{"other"}};

    CHECK_FALSE(ownsMessageContentResult({
        .requestAccountId = "account",
        .requestEmailId = "email",
        .activeAccountId = std::string_view{"account"},
        .selectedEmailIds = changed,
        .routeAccountId = std::nullopt,
        .routeEmailId = std::nullopt,
    }));
    CHECK_FALSE(ownsMessageContentResult({
        .requestAccountId = "account",
        .requestEmailId = "email",
        .activeAccountId = std::string_view{"account"},
        .selectedEmailIds = multiple,
        .routeAccountId = std::nullopt,
        .routeEmailId = std::nullopt,
    }));
}

TEST_CASE("routed message content owns the detail even without list selection")
{
    CHECK(ownsMessageContentResult({
        .requestAccountId = "account",
        .requestEmailId = "email",
        .activeAccountId = std::string_view{"account"},
        .selectedEmailIds = {},
        .routeAccountId = std::string_view{"account"},
        .routeEmailId = std::string_view{"email"},
    }));
}

TEST_CASE("message content never applies across active accounts")
{
    const std::array selected{std::string{"email"}};
    CHECK_FALSE(ownsMessageContentResult({
        .requestAccountId = "account",
        .requestEmailId = "email",
        .activeAccountId = std::string_view{"other-account"},
        .selectedEmailIds = selected,
        .routeAccountId = std::string_view{"account"},
        .routeEmailId = std::string_view{"email"},
    }));
}
