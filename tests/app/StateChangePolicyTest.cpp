#include "app/StateChangePolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("state-change subscriptions include supported groupware data types", "[app][sync]")
{
    CHECK(javelin::app::subscribedStateChangeTypes({.calendar = false, .contacts = false}) ==
          std::vector<std::string>{"Email", "Mailbox"});
    CHECK(javelin::app::subscribedStateChangeTypes({.calendar = false, .contacts = true}) ==
          std::vector<std::string>{"Email", "Mailbox", "AddressBook", "ContactCard"});
    CHECK(javelin::app::subscribedStateChangeTypes({.calendar = true, .contacts = true}) ==
          std::vector<std::string>{"Email", "Mailbox", "Calendar", "CalendarEvent", "AddressBook",
                                   "ContactCard"});
}

TEST_CASE("contact state changes are routed away from mail refresh", "[app][sync]")
{
    auto routed = javelin::app::routeStateChanges({
        {"Email", "mail-2"},
        {"Mailbox", "boxes-2"},
        {"CalendarEvent", "calendar-2"},
        {"AddressBook", "books-2"},
        {"ContactCard", "contacts-2"},
    });

    CHECK(routed.calendarChanged);
    CHECK(routed.contactsChanged);
    CHECK(routed.mailStates == std::unordered_map<std::string, std::string>{
                                   {"Email", "mail-2"}, {"Mailbox", "boxes-2"}});
}
