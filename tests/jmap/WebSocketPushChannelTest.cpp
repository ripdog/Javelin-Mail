#include "jmap/sync/WebSocketPushChannel.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("WebSocket push enable uses the requested data types", "[jmap][push][websocket]")
{
    const auto encoded = javelin::jmap::sync::encodeWebSocketPushEnable({
        .accountId = "account-1",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox", "Calendar", "CalendarEvent"},
        .groupwareAccountIds = {},
    });

    REQUIRE(encoded.has_value());
    CHECK(encoded->find(R"("@type":"WebSocketPushEnable")") != std::string::npos);
    CHECK(encoded->find(R"("dataTypes":["Email","Mailbox","Calendar","CalendarEvent"])") !=
          std::string::npos);
    CHECK(encoded->find(R"("pushState":"push-state-1")") != std::string::npos);
}

TEST_CASE("state-change routing includes groupware changes from secondary accounts", "[jmap][push]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "mail-account",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox", "AddressBook", "ContactCard"},
        .groupwareAccountIds = {"mail-account", "contacts-account"},
    };
    const javelin::jmap::sync::AccountTypeStateMap changed{
        {"mail-account", {{"Email", "mail-2"}, {"Mailbox", "boxes-2"}}},
        {"contacts-account", {{"ContactCard", "contacts-2"}}},
        {"unrelated-account", {{"ContactCard", "unrelated-2"}, {"Email", "other-mail-2"}}},
    };

    CHECK(javelin::jmap::sync::subscribedStateChanges(subscription, changed) ==
          javelin::jmap::sync::AccountTypeStateMap{
              {"mail-account", {{"Email", "mail-2"}, {"Mailbox", "boxes-2"}}},
              {"contacts-account", {{"ContactCard", "contacts-2"}}}});
}
