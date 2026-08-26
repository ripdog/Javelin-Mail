#include "gui/shell/TabActivationPolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("empty tab activation restores the mailbox pane without refreshing",
          "[gui][tabs][activation]")
{
    const auto plan = javelin::gui::shell::planTabActivation({});

    CHECK(plan.showMailboxPane);
    CHECK_FALSE(plan.refreshRemote);
}

TEST_CASE("mailbox activation preserves a current loaded prefix and refreshes only when needed",
          "[gui][tabs][activation][infinite-scroll]")
{
    const auto home = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Mailbox,
        .homeTab = true,
        .messageListStale = false,
        .remoteRefreshRequested = false,
    });
    const auto secondary = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Mailbox,
        .homeTab = false,
        .messageListStale = false,
        .remoteRefreshRequested = false,
    });
    const auto stale = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Mailbox,
        .messageListStale = true,
    });
    const auto requested = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Mailbox,
        .remoteRefreshRequested = true,
    });

    CHECK(home.showMailboxPane);
    CHECK_FALSE(secondary.showMailboxPane);
    CHECK_FALSE(home.refreshRemote);
    CHECK_FALSE(secondary.refreshRemote);
    CHECK(stale.refreshRemote);
    CHECK(requested.refreshRemote);
}

TEST_CASE("search activation refreshes only stale or explicitly requested results",
          "[gui][tabs][activation]")
{
    const auto cached = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Search,
        .homeTab = false,
        .messageListStale = false,
        .remoteRefreshRequested = false,
    });
    const auto stale = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Search,
        .homeTab = false,
        .messageListStale = true,
        .remoteRefreshRequested = false,
    });
    const auto requested = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Search,
        .homeTab = false,
        .messageListStale = false,
        .remoteRefreshRequested = true,
    });

    CHECK_FALSE(cached.refreshRemote);
    CHECK(stale.refreshRemote);
    CHECK(requested.refreshRemote);
}

TEST_CASE("compose activation hides mail chrome without refreshing mail", "[gui][tabs][activation]")
{
    const auto plan = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Compose,
        .homeTab = true,
        .messageListStale = true,
        .remoteRefreshRequested = true,
    });

    CHECK_FALSE(plan.showMailboxPane);
    CHECK_FALSE(plan.refreshRemote);
}

TEST_CASE("contacts and calendar activation hide mail chrome and refresh only on request",
          "[gui][tabs][activation]")
{
    const auto contacts = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Contacts,
        .remoteRefreshRequested = false,
    });
    const auto calendar = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Calendar,
        .remoteRefreshRequested = true,
    });

    CHECK_FALSE(contacts.showMailboxPane);
    CHECK_FALSE(contacts.refreshRemote);
    CHECK_FALSE(calendar.showMailboxPane);
    CHECK(calendar.refreshRemote);
}
