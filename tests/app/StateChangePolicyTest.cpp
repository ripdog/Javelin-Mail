#include "app/StateChangePolicy.h"
#include "jmap/sync/StateChangeSource.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("state-change subscriptions include supported groupware data types", "[app][sync]")
{
    CHECK(javelin::app::subscribedStateChangeTypes(
              {.calendar = false, .contacts = false, .identities = false}) ==
          std::vector<std::string>{"Email", "Mailbox"});
    CHECK(javelin::app::subscribedStateChangeTypes(
              {.calendar = false, .contacts = true, .identities = true}) ==
          std::vector<std::string>{"Email", "Mailbox", "Identity", "AddressBook", "ContactCard"});
    CHECK(javelin::app::subscribedStateChangeTypes(
              {.calendar = true, .contacts = true, .identities = true}) ==
          std::vector<std::string>{"Email", "Mailbox", "Identity", "Calendar", "CalendarEvent",
                                   "CalendarEventNotification", "AddressBook", "ContactCard"});
}

TEST_CASE("secondary Identity changes survive subscription filtering", "[app][sync][identity]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "owner-account",
        .lastState = {},
        .types = {"Email", "Mailbox", "Identity"},
        .groupwareAccountIds = {"owner-account", "identity-account"},
    };
    const auto filtered = javelin::jmap::sync::subscribedStateChanges(
        subscription,
        {{"owner-account", {{"Email", "mail-2"}}},
         {"identity-account", {{"Identity", "identity-2"}, {"Email", "secondary-mail-2"}}}});

    CHECK(filtered == javelin::jmap::sync::AccountTypeStateMap{
                          {"owner-account", {{"Email", "mail-2"}}},
                          {"identity-account", {{"Identity", "identity-2"}}}});
}

TEST_CASE("contact state changes are routed away from mail refresh", "[app][sync]")
{
    auto routed = javelin::app::routeStateChanges(
        {
            {"mail-account",
             {{"Email", "mail-2"},
              {"Mailbox", "boxes-2"},
              {"Identity", "identities-2"},
              {"CalendarEvent", "calendar-2"},
              {"CalendarEventNotification", "notifications-2"}}},
            {"contacts-account", {{"AddressBook", "books-2"}, {"ContactCard", "contacts-2"}}},
        },
        "mail-account");

    CHECK(routed.mailStates == std::unordered_map<std::string, std::string>{
                                   {"Email", "mail-2"}, {"Mailbox", "boxes-2"}});
    CHECK(routed.calendarStates == javelin::jmap::sync::AccountTypeStateMap{
                                       {"mail-account",
                                        {{"CalendarEvent", "calendar-2"},
                                         {"CalendarEventNotification", "notifications-2"}}}});
    CHECK(routed.contactStates ==
          javelin::jmap::sync::AccountTypeStateMap{
              {"contacts-account", {{"AddressBook", "books-2"}, {"ContactCard", "contacts-2"}}}});
    CHECK(routed.identityStates == javelin::jmap::sync::AccountTypeStateMap{
                                       {"mail-account", {{"Identity", "identities-2"}}}});
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
    CHECK_FALSE(javelin::app::shouldDeferForActiveMutation("CalendarEventNotification"));
}

TEST_CASE("mail state changes refresh every affected watched mailbox window", "[app][sync][mail]")
{
    const std::vector<std::string> affectedMailboxIds{"inbox", "junk"};

    CHECK(javelin::app::shouldRefreshMailboxWindow(false, affectedMailboxIds, "junk"));
    CHECK_FALSE(javelin::app::shouldRefreshMailboxWindow(false, affectedMailboxIds, "archive"));
    CHECK(javelin::app::shouldRefreshMailboxWindow(true, affectedMailboxIds, "archive"));
}

TEST_CASE("mailbox watch updates only materialize newly watched mailboxes", "[app][sync][mail]")
{
    const std::vector<std::pair<std::string, std::string>> previous{{"inbox", "Inbox"},
                                                                    {"archive", "Archive"}};
    const std::vector<std::pair<std::string, std::string>> removed{{"inbox", "Inbox"}};
    const std::vector<std::pair<std::string, std::string>> added{{"inbox", "Inbox"},
                                                                 {"sent", "Sent"}};

    CHECK(javelin::app::newlyWatchedMailboxIds(previous, removed).empty());
    CHECK(javelin::app::newlyWatchedMailboxIds(previous, added) ==
          std::vector<std::string>{"sent"});

    const std::vector<std::string> explicitlyRequested{"sent"};
    CHECK(javelin::app::shouldRefreshMailboxWindow(false, std::vector<std::string>{},
                                                   explicitlyRequested, "sent"));
    CHECK_FALSE(javelin::app::shouldRefreshMailboxWindow(false, std::vector<std::string>{},
                                                         explicitlyRequested, "archive"));
}
