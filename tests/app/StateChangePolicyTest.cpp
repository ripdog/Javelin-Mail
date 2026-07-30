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
    auto routed = javelin::app::routeStateChanges(
        {
            {"mail-account",
             {{"Email", "mail-2"}, {"Mailbox", "boxes-2"}, {"CalendarEvent", "calendar-2"}}},
            {"contacts-account", {{"AddressBook", "books-2"}, {"ContactCard", "contacts-2"}}},
        },
        "mail-account");

    CHECK(routed.mailStates == std::unordered_map<std::string, std::string>{
                                   {"Email", "mail-2"}, {"Mailbox", "boxes-2"}});
    CHECK(routed.calendarStates == javelin::jmap::sync::AccountTypeStateMap{
                                       {"mail-account", {{"CalendarEvent", "calendar-2"}}}});
    CHECK(routed.contactStates ==
          javelin::jmap::sync::AccountTypeStateMap{
              {"contacts-account", {{"AddressBook", "books-2"}, {"ContactCard", "contacts-2"}}}});
}

TEST_CASE("unfinished contact refresh jobs are restored after restart", "[app][sync]")
{
    CHECK(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::Queued));
    CHECK(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::Paused));
    CHECK(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::WaitingForNetwork));
    CHECK(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::WaitingForAuth));
    CHECK_FALSE(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::Running));
    CHECK_FALSE(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::Failed));
    CHECK_FALSE(javelin::app::shouldRestoreContactRefresh(javelin::app::WorkStatus::Complete));
}

TEST_CASE("contact refresh rebases active mutation projections", "[app][sync][contacts]")
{
    CHECK_FALSE(javelin::app::shouldDeferForActiveMutation("AddressBook"));
    CHECK_FALSE(javelin::app::shouldDeferForActiveMutation("ContactCard"));
    CHECK(javelin::app::shouldDeferForActiveMutation("Calendar"));
    CHECK(javelin::app::shouldDeferForActiveMutation("CalendarEvent"));
}
