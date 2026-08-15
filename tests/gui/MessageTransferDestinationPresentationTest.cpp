#include "gui/shell/MessageTransferDestinationPresentation.h"

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    [[nodiscard]] javelin::jmap::cache::CachedAccount
    account(std::string id, std::string connection, std::string remoteId, std::string name,
            bool hasMail = true, bool readOnly = false)
    {
        return {
            .accountId = std::move(id),
            .connectionId = std::move(connection),
            .remoteAccountId = std::move(remoteId),
            .name = std::move(name),
            .isPersonal = true,
            .isReadOnly = readOnly,
            .isPrimary = false,
            .hasMailCapability = hasMail,
            .mayCreateTopLevelMailbox = true,
            .ownerAccountId = {},
            .hasSubmissionCapability = false,
            .maxDelayedSendSeconds = 0,
        };
    }

    [[nodiscard]] javelin::jmap::cache::MailboxTreeItem
    mailbox(std::string id, std::string name, std::optional<std::string> role = std::nullopt,
            bool writable = true, std::optional<std::string> parentId = std::nullopt)
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .parentId = std::move(parentId),
            .role = std::move(role),
            .sortOrder = 0,
            .totalEmails = 0,
            .unreadEmails = 0,
            .totalThreads = 0,
            .unreadThreads = 0,
            .isSubscribed = true,
            .myRights =
                {
                    .mayReadItems = true,
                    .mayAddItems = writable,
                    .mayRemoveItems = true,
                    .maySetSeen = true,
                    .maySetKeywords = true,
                    .mayCreateChild = true,
                    .mayRename = true,
                    .mayDelete = true,
                    .maySubmit = true,
                },
            .hasChildren = false,
            .pendingCreate = false,
        };
    }
} // namespace

TEST_CASE(
    "transfer destination presentation keeps current account inline and other accounts grouped",
    "[gui][mail-transfer][destination]")
{
    const std::vector accounts{
        account("local-a", "connection-a", "u1", "Source server"),
        account("local-b", "connection-b", "u1", "Destination server"),
        account("local-readonly", "connection-c", "u3", "Read only", true, true),
        account("local-calendar", "connection-d", "u4", "Calendar only", false, false),
    };
    const std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
        mailboxes{
            {"local-a",
             {mailbox("inbox", "Inbox", std::string{"inbox"}),
              mailbox("locked", "Locked", std::nullopt, false), mailbox("projects", "Projects")}},
            {"local-b",
             {mailbox("archive", "Archive", std::string{"archive"}), mailbox("parent", "Parent"),
              mailbox("child", "Child", std::nullopt, true, std::string{"parent"})}},
            {"local-readonly", {mailbox("readonly", "Read only")}},
            {"local-calendar", {mailbox("calendar-mailbox", "Impossible")}},
        };

    const auto presentation = javelin::gui::shell::buildMessageTransferDestinationPresentation(
        "local-a", accounts, mailboxes, [](const QStringView accountId)
        { return accountId == QStringLiteral("local-b") ? QStringLiteral("Work") : QString{}; });

    REQUIRE(presentation.currentAccountRows.size() == 2);
    CHECK(presentation.currentAccountRows.at(0).accountId == "local-a");
    CHECK(presentation.currentAccountRows.at(0).mailboxId == "inbox");
    CHECK_FALSE(presentation.currentAccountRows.at(0).separatorBefore);
    CHECK(presentation.currentAccountRows.at(1).mailboxId == "projects");
    CHECK(presentation.currentAccountRows.at(1).separatorBefore);

    REQUIRE(presentation.otherAccounts.size() == 1);
    const auto& destinationAccount = presentation.otherAccounts.front();
    CHECK(destinationAccount.accountId == "local-b");
    CHECK(destinationAccount.label == QStringLiteral("Work"));
    REQUIRE(destinationAccount.rows.size() == 3);
    CHECK(destinationAccount.rows.at(0).mailboxId == "archive");
    CHECK(destinationAccount.rows.at(1).mailboxId == "parent");
    CHECK(destinationAccount.rows.at(1).separatorBefore);
    CHECK(destinationAccount.rows.at(2).mailboxId == "child");
    CHECK(destinationAccount.rows.at(2).depth == 1);
}

TEST_CASE("transfer destination presentation disambiguates duplicate configured account names",
          "[gui][mail-transfer][destination][accounts]")
{
    const std::vector accounts{
        account("source", "connection-source", "src", "Source"),
        account("account-a", "connection-a", "u1", "Personal A"),
        account("account-b", "connection-b", "u2", "Personal B"),
        account("account-c-123456789", "connection-c", "u2", "Personal B"),
    };
    const std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
        mailboxes{
            {"source", {mailbox("inbox", "Inbox")}},
            {"account-a", {mailbox("a", "A")}},
            {"account-b", {mailbox("b", "B")}},
            {"account-c-123456789", {mailbox("c", "C")}},
        };

    const auto presentation = javelin::gui::shell::buildMessageTransferDestinationPresentation(
        "source", accounts, mailboxes,
        [](const QStringView accountId)
        {
            return accountId == QStringLiteral("source") ? QStringLiteral("Source")
                                                         : QStringLiteral("Personal");
        });
    REQUIRE(presentation.otherAccounts.size() == 3);
    CHECK(presentation.otherAccounts.at(0).label == QStringLiteral("Personal — Personal A"));
    CHECK(presentation.otherAccounts.at(1).label == QStringLiteral("Personal — Personal B"));
    CHECK(presentation.otherAccounts.at(2).label.startsWith(QStringLiteral("Personal —")));
    CHECK(presentation.otherAccounts.at(1).label != presentation.otherAccounts.at(2).label);
}

TEST_CASE("transfer destination presentation omits accounts without writable mailboxes",
          "[gui][mail-transfer][destination][rights]")
{
    const std::vector accounts{
        account("source", "connection-source", "src", "Source"),
        account("locked", "connection-locked", "locked", "Locked"),
    };
    const std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
        mailboxes{
            {"source", {mailbox("inbox", "Inbox")}},
            {"locked", {mailbox("locked-mailbox", "Locked", std::nullopt, false)}},
        };

    const auto presentation = javelin::gui::shell::buildMessageTransferDestinationPresentation(
        "source", accounts, mailboxes);
    CHECK(presentation.otherAccounts.empty());
}
