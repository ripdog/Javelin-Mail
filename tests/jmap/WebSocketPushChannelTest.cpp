#include "jmap/sync/WebSocketPushChannel.h"

#include "jmap/sync/PushProtocol.h"

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

TEST_CASE("WebSocket push messages use the shared protocol parser", "[jmap][push][websocket]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "account-1",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox"},
        .groupwareAccountIds = {},
    };
    auto parsed = javelin::jmap::sync::parseWebSocketPushMessage(
        subscription, subscription.lastState,
        R"({"@type":"StateChange","changed":{"account-1":{"Email":"email-2"}},"pushState":"push-state-2"})");

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeEvent>(parsed));
    const auto& event = std::get<javelin::jmap::sync::StateChangeEvent>(parsed);
    CHECK(event.newState == "push-state-2");
    CHECK(event.changedTypes == std::vector<std::string>{"Email"});
    CHECK(event.changedStates ==
          javelin::jmap::sync::AccountTypeStateMap{{"account-1", {{"Email", "email-2"}}}});
}
