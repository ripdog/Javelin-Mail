#include "app/MessageNavigationCoordinator.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("message navigation routes are superseded and cleared by identity", "[app][navigation]")
{
    javelin::app::MessageNavigationCoordinator coordinator;

    const auto first = coordinator.openEmail("account-1", "mailbox-1", "thread-1", "email-1");
    REQUIRE(coordinator.currentRoute().has_value());
    CHECK(coordinator.currentRoute()->id == first);
    CHECK(coordinator.currentRoute()->emailId == "email-1");

    const auto second = coordinator.openEmail("account-1", "mailbox-2", std::nullopt, "email-2");
    REQUIRE(coordinator.currentRoute().has_value());
    CHECK(coordinator.currentRoute()->id == second);
    CHECK(coordinator.currentRoute()->mailboxId == "mailbox-2");

    coordinator.complete(first);
    REQUIRE(coordinator.currentRoute().has_value());
    CHECK(coordinator.currentRoute()->id == second);

    coordinator.complete(second);
    CHECK_FALSE(coordinator.currentRoute().has_value());
}

TEST_CASE("message navigation cancellation clears the active route", "[app][navigation]")
{
    javelin::app::MessageNavigationCoordinator coordinator;
    static_cast<void>(coordinator.openEmail("account-1", "mailbox-1", "thread-1", "email-1"));

    coordinator.cancel();

    CHECK_FALSE(coordinator.currentRoute().has_value());
}
