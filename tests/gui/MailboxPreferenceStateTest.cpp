#include "gui/mailboxes/MailboxTreeModel.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("mailbox Hide clears offline and notification preferences", "[gui][mailbox][preferences]")
{
    const auto hidden = javelin::gui::mailboxes::withMailboxPreference(
        {.offline = true, .notifications = true, .hidden = false},
        javelin::gui::mailboxes::MailboxPreference::Hidden, true);

    CHECK(hidden.hidden);
    CHECK_FALSE(hidden.offline);
    CHECK_FALSE(hidden.notifications);
}

TEST_CASE("mailbox background preferences automatically show a hidden mailbox",
          "[gui][mailbox][preferences]")
{
    const auto offline = javelin::gui::mailboxes::withMailboxPreference(
        {.offline = false, .notifications = false, .hidden = true},
        javelin::gui::mailboxes::MailboxPreference::Offline, true);
    CHECK(offline.offline);
    CHECK_FALSE(offline.hidden);

    const auto notifications = javelin::gui::mailboxes::withMailboxPreference(
        {.offline = false, .notifications = false, .hidden = true},
        javelin::gui::mailboxes::MailboxPreference::Notifications, true);
    CHECK(notifications.notifications);
    CHECK_FALSE(notifications.hidden);
}

TEST_CASE("showing a mailbox does not restore previous background preferences",
          "[gui][mailbox][preferences]")
{
    auto state = javelin::gui::mailboxes::withMailboxPreference(
        {.offline = true, .notifications = true, .hidden = false},
        javelin::gui::mailboxes::MailboxPreference::Hidden, true);
    state = javelin::gui::mailboxes::withMailboxPreference(
        state, javelin::gui::mailboxes::MailboxPreference::Hidden, false);

    CHECK_FALSE(state.hidden);
    CHECK_FALSE(state.offline);
    CHECK_FALSE(state.notifications);
}
