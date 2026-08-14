#include "gui/mailboxes/MailboxSort.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] javelin::jmap::cache::MailboxTreeItem
    mailbox(std::string id, std::string name, std::optional<std::string> role = std::nullopt)
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .parentId = std::nullopt,
            .role = std::move(role),
            .myRights = {},
        };
    }
} // namespace

TEST_CASE("mailbox display order puts special-use mailboxes before alphabetical mailboxes",
          "[gui][mailbox]")
{
    const std::vector mailboxes{
        mailbox("zebra", "Zebra"),
        mailbox("trash", "Deleted", "trash"),
        mailbox("alpha", "alpha"),
        mailbox("sent", "Outbox", "sent"),
        mailbox("inbox", "Primary", "inbox"),
        mailbox("archive", "Saved", "archive"),
        mailbox("drafts", "Composing", "drafts"),
        mailbox("scheduled", "Later", "scheduled"),
        mailbox("junk", "Spam", "junk"),
    };

    const auto ordered = javelin::gui::mailboxes::mailboxesInDisplayOrder(mailboxes);
    std::vector<std::string> ids;
    ids.reserve(ordered.size());
    for (const auto* item : ordered)
    {
        ids.push_back(item->id);
    }

    CHECK(ids == std::vector<std::string>{"inbox", "archive", "drafts", "scheduled", "sent", "junk",
                                          "trash", "alpha", "zebra"});
}

TEST_CASE("transfer destinations retain every writable mailbox including the open mailbox",
          "[gui][mailbox][transfer]")
{
    auto inbox = mailbox("inbox", "Inbox", "inbox");
    inbox.myRights.mayAddItems = true;
    auto archive = mailbox("archive", "Archive", "archive");
    archive.myRights.mayAddItems = true;
    auto readOnly = mailbox("shared", "Shared");
    readOnly.myRights.mayAddItems = false;

    const std::vector mailboxes{inbox, archive, readOnly};
    const auto destinations = javelin::gui::mailboxes::writableMailboxesInDisplayOrder(mailboxes);

    REQUIRE(destinations.size() == 2);
    CHECK(destinations[0]->id == "inbox");
    CHECK(destinations[1]->id == "archive");
}
