#include "gui/mailboxes/MailboxPresentation.h"

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

    const auto presentation =
        javelin::gui::mailboxes::buildMailboxPresentation("account-1", mailboxes);
    const auto ordered = javelin::gui::mailboxes::flattenMailboxPresentation(presentation);
    std::vector<std::string> ids;
    ids.reserve(ordered.size());
    for (const auto& row : ordered)
    {
        ids.push_back(row.node->mailbox.id);
    }

    CHECK(ids == std::vector<std::string>{"inbox", "archive", "drafts", "scheduled", "sent", "junk",
                                          "trash", "alpha", "zebra"});
}

TEST_CASE("mailbox presentation carries hierarchy grouping and account identity",
          "[gui][mailbox][presentation]")
{
    auto inbox = mailbox("inbox", "Inbox", "inbox");
    inbox.myRights.mayAddItems = true;
    auto archive = mailbox("archive", "Archive", "archive");
    archive.myRights.mayAddItems = true;
    auto projects = mailbox("projects", "Projects");
    auto child = mailbox("child", "Alpha");
    child.parentId = "projects";
    auto zebra = mailbox("zebra", "Zebra");

    const std::vector mailboxes{child, zebra, projects, archive, inbox};
    const auto presentation =
        javelin::gui::mailboxes::buildMailboxPresentation("account-1", mailboxes);
    const auto rows = javelin::gui::mailboxes::flattenMailboxPresentation(presentation);

    REQUIRE(rows.size() == 5);
    CHECK(rows[0].node->mailbox.id == "inbox");
    CHECK(rows[0].node->accountId == "account-1");
    CHECK(rows[0].node->group == javelin::gui::mailboxes::MailboxPresentationGroup::SpecialUse);
    CHECK_FALSE(rows[0].separatorBefore);
    CHECK(rows[1].node->mailbox.id == "archive");
    CHECK(rows[2].node->mailbox.id == "projects");
    CHECK(rows[2].separatorBefore);
    CHECK(rows[2].depth == 0);
    CHECK(rows[3].node->mailbox.id == "child");
    CHECK(rows[3].depth == 1);
    CHECK(rows[3].node->group == javelin::gui::mailboxes::MailboxPresentationGroup::User);
    CHECK(rows[4].node->mailbox.id == "zebra");
    CHECK_FALSE(rows[4].separatorBefore);
}
