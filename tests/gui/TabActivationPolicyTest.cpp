#include "gui/shell/TabActivationPolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("empty tab activation clears message presentation", "[gui][tabs][activation]")
{
    const auto plan = javelin::gui::shell::planTabActivation({});

    CHECK(plan.showMailboxPane);
    CHECK(plan.clearMessagePresentation);
    CHECK_FALSE(plan.refreshRemote);
}

TEST_CASE("mailbox activation shows navigation only for the home tab", "[gui][tabs][activation]")
{
    const auto home = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Mailbox,
        .homeTab = true,
    });
    const auto secondary = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Mailbox,
        .homeTab = false,
    });

    CHECK(home.showMailboxPane);
    CHECK_FALSE(secondary.showMailboxPane);
    CHECK(home.refreshRemote);
    CHECK(secondary.refreshRemote);
}

TEST_CASE("search activation refreshes only stale or explicitly requested results",
          "[gui][tabs][activation]")
{
    const auto cached = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Search,
        .homeTab = false,
        .messagePageStale = false,
        .remoteRefreshRequested = false,
    });
    const auto stale = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Search,
        .homeTab = false,
        .messagePageStale = true,
        .remoteRefreshRequested = false,
    });
    const auto requested = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Search,
        .homeTab = false,
        .messagePageStale = false,
        .remoteRefreshRequested = true,
    });

    CHECK_FALSE(cached.refreshRemote);
    CHECK(stale.refreshRemote);
    CHECK(requested.refreshRemote);
}

TEST_CASE("compose activation preserves the message presentation behind the editor",
          "[gui][tabs][activation]")
{
    const auto plan = javelin::gui::shell::planTabActivation({
        .kind = javelin::gui::shell::TabKind::Compose,
        .homeTab = true,
        .messagePageStale = true,
        .remoteRefreshRequested = true,
    });

    CHECK_FALSE(plan.showMailboxPane);
    CHECK_FALSE(plan.clearMessagePresentation);
    CHECK_FALSE(plan.refreshRemote);
}

TEST_CASE("contacts and calendar activation clear mail and refresh only on request",
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

    CHECK(contacts.clearMessagePresentation);
    CHECK_FALSE(contacts.refreshRemote);
    CHECK(calendar.clearMessagePresentation);
    CHECK(calendar.refreshRemote);
}
