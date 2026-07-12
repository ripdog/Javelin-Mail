#include "jmap/sync/MailboxInterestRegistry.h"

#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

TEST_CASE("mailbox interests retain one mailbox while any observer remains", "[jmap][sync]")
{
    javelin::jmap::sync::MailboxInterestRegistry registry;

    const auto first = registry.observe("account-1", "mailbox-inbox");
    const auto second = registry.observe("account-1", "mailbox-inbox");

    CHECK(registry.observationCount("account-1", "mailbox-inbox") == 2);
    CHECK(registry.mailboxIds("account-1") == std::vector<std::string>{"mailbox-inbox"});

    REQUIRE(registry.unobserve(first).has_value());
    CHECK(registry.observationCount("account-1", "mailbox-inbox") == 1);
    CHECK(registry.mailboxIds("account-1") == std::vector<std::string>{"mailbox-inbox"});

    REQUIRE(registry.unobserve(second).has_value());
    CHECK(registry.observationCount("account-1", "mailbox-inbox") == 0);
    CHECK(registry.mailboxIds("account-1").empty());
}

TEST_CASE("mailbox interests are account scoped and stable", "[jmap][sync]")
{
    javelin::jmap::sync::MailboxInterestRegistry registry;

    static_cast<void>(registry.observe("account-1", "mailbox-z"));
    static_cast<void>(registry.observe("account-2", "mailbox-shared"));
    static_cast<void>(registry.observe("account-1", "mailbox-a"));
    static_cast<void>(registry.observe("account-1", "mailbox-z"));

    CHECK(registry.mailboxIds("account-1") == std::vector<std::string>{"mailbox-a", "mailbox-z"});
    CHECK(registry.mailboxIds("account-2") == std::vector<std::string>{"mailbox-shared"});

    registry.eraseAccountsNotIn(std::unordered_set<std::string>{"account-2"});
    CHECK(registry.mailboxIds("account-1").empty());
    CHECK(registry.mailboxIds("account-2") == std::vector<std::string>{"mailbox-shared"});
}
