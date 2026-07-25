#include "gui/shell/MessageActionPolicy.h"

#include <catch2/catch_test_macros.hpp>

using namespace javelin::gui::shell;

TEST_CASE("message actions are disabled without an active selection")
{
    const auto actions = messageActionAvailability({});

    CHECK(actions.newMessage);
    CHECK_FALSE(actions.reply);
    CHECK_FALSE(actions.archive);
    CHECK_FALSE(actions.permanentDelete);
    CHECK_FALSE(actions.viewSource);
}

TEST_CASE("mailbox selections enable mailbox and movable actions")
{
    const auto actions = messageActionAvailability({
        .tabKind = TabKind::Mailbox,
        .hasAccount = true,
        .hasMailbox = true,
        .selectedCount = 2,
        .hasReadSelection = true,
    });

    CHECK(actions.reply);
    CHECK(actions.replyAll);
    CHECK(actions.forward);
    CHECK(actions.archive);
    CHECK(actions.markUnread);
    CHECK(actions.deleteFromMailbox);
    CHECK(actions.permanentDelete);
    CHECK(actions.move);
    CHECK(actions.copy);
    CHECK(actions.viewSource);
    CHECK_FALSE(actions.editDraft);
}

TEST_CASE("one draft in the drafts mailbox can be edited")
{
    const auto actions = messageActionAvailability({
        .tabKind = TabKind::Mailbox,
        .hasAccount = true,
        .hasMailbox = true,
        .selectedCount = 1,
        .activeMailboxIsDrafts = true,
    });

    CHECK(actions.editDraft);
}

TEST_CASE("search selections can move but cannot use mailbox deletion")
{
    const auto actions = messageActionAvailability({
        .tabKind = TabKind::Search,
        .hasAccount = true,
        .selectedCount = 1,
    });

    CHECK(actions.reply);
    CHECK(actions.archive);
    CHECK(actions.move);
    CHECK(actions.copy);
    CHECK_FALSE(actions.deleteFromMailbox);
    CHECK_FALSE(actions.editDraft);
}

TEST_CASE("compose preserves source actions but disables reply commands")
{
    const auto actions = messageActionAvailability({
        .tabKind = TabKind::Compose,
        .hasAccount = true,
        .selectedCount = 1,
    });

    CHECK_FALSE(actions.reply);
    CHECK_FALSE(actions.replyAll);
    CHECK_FALSE(actions.forward);
    CHECK(actions.permanentDelete);
    CHECK(actions.viewSource);
    CHECK_FALSE(actions.archive);
}

TEST_CASE("contacts never expose message actions")
{
    const auto actions = messageActionAvailability({
        .tabKind = TabKind::Contacts,
        .hasAccount = true,
        .selectedCount = 1,
        .hasReadSelection = true,
    });

    CHECK_FALSE(actions.reply);
    CHECK_FALSE(actions.markUnread);
    CHECK_FALSE(actions.permanentDelete);
    CHECK_FALSE(actions.viewSource);
}
