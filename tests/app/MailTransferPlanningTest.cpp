#include "app/MailTransferPlanning.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace
{
    using namespace javelin::app;
    using javelin::jmap::cache::CachedAccount;
    using javelin::jmap::cache::MailboxTreeItem;
    using javelin::jmap::domain::Email;
    using javelin::jmap::domain::MailboxRights;

    [[nodiscard]] CachedAccount account(std::string localId, std::string connectionId,
                                        std::string remoteId)
    {
        return {
            .accountId = std::move(localId),
            .connectionId = std::move(connectionId),
            .remoteAccountId = std::move(remoteId),
            .name = "Account",
            .isPersonal = true,
            .isReadOnly = false,
            .isPrimary = true,
            .hasMailCapability = true,
            .mayCreateTopLevelMailbox = true,
            .ownerAccountId = {},
            .hasSubmissionCapability = false,
            .maxDelayedSendSeconds = 0,
        };
    }

    [[nodiscard]] MailboxTreeItem mailbox(std::string id, std::string name,
                                          MailboxRights rights = {.mayReadItems = true,
                                                                  .mayAddItems = true,
                                                                  .mayRemoveItems = true,
                                                                  .maySetSeen = true,
                                                                  .maySetKeywords = true})
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .parentId = std::nullopt,
            .role = std::nullopt,
            .sortOrder = 0,
            .totalEmails = 0,
            .unreadEmails = 0,
            .totalThreads = 0,
            .unreadThreads = 0,
            .isSubscribed = true,
            .myRights = rights,
            .hasChildren = false,
            .pendingCreate = false,
        };
    }

    [[nodiscard]] Email email(std::string id, std::vector<std::string> mailboxIds,
                              std::vector<std::string> keywords = {"$seen", "$flagged"})
    {
        return {
            .id = std::move(id),
            .blobId = "blob-1",
            .threadId = "thread-1",
            .mailboxIds = std::move(mailboxIds),
            .keywords = std::move(keywords),
            .size = 4096,
            .receivedAt = "2026-08-15T08:00:00Z",
            .sentAt = std::nullopt,
            .messageId = {},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = false,
            .subject = "Transfer",
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = "Preview",
        };
    }

    [[nodiscard]] PlannedMailTransfer requirePlan(MailTransferPlanResult result)
    {
        if (const auto* error = std::get_if<QString>(&result))
            FAIL(error->toStdString());
        return std::get<PlannedMailTransfer>(std::move(result));
    }
} // namespace

TEST_CASE("cross-server copy preserves source metadata without source cleanup",
          "[app][mail-transfer][planning]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox", "important"});
    const auto plan = requirePlan(planMailTransfer(
        {.sourceAccountId = source.accountId,
         .sourceMailboxId = std::optional<std::string>{"inbox"},
         .destinationAccountId = destination.accountId,
         .destinationMailboxId = "archive",
         .operation = MailTransferOperation::Copy},
        {message.id}, {message}, {mailbox("inbox", "Inbox"), mailbox("important", "Important")},
        {mailbox("archive", "Archive")}, source, destination));

    CHECK(plan.topology == MailTransferTopology::CrossServerImport);
    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items.front().sourceBlobId == message.blobId);
    CHECK(plan.items.front().sourceMailboxIds == message.mailboxIds);
    CHECK(plan.items.front().sourceKeywords == message.keywords);
    CHECK(plan.items.front().sourceReceivedAt == message.receivedAt);
    CHECK(plan.items.front().sourceSize == message.size);
    CHECK(plan.items.front().sourceRemoveMailboxIds.empty());
    CHECK_FALSE(plan.items.front().sourceDestroy);
}

TEST_CASE("accounts on one JMAP connection use Email copy topology",
          "[app][mail-transfer][planning]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-a", "u2");
    const auto message = email("email-1", {"inbox"}, {});
    const auto plan =
        requirePlan(planMailTransfer({.sourceAccountId = source.accountId,
                                      .sourceMailboxId = std::optional<std::string>{"inbox"},
                                      .destinationAccountId = destination.accountId,
                                      .destinationMailboxId = "archive",
                                      .operation = MailTransferOperation::Copy},
                                     {message.id}, {message}, {mailbox("inbox", "Inbox")},
                                     {mailbox("archive", "Archive")}, source, destination));
    CHECK(plan.topology == MailTransferTopology::SameSessionCopy);
}

TEST_CASE("cross-account move removes only the actual selection mailbox when resident there",
          "[app][mail-transfer][planning][move]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox", "important"}, {});
    const auto plan = requirePlan(planMailTransfer(
        {.sourceAccountId = source.accountId,
         .sourceMailboxId = std::optional<std::string>{"inbox"},
         .destinationAccountId = destination.accountId,
         .destinationMailboxId = "inbox",
         .operation = MailTransferOperation::Move},
        {message.id}, {message}, {mailbox("inbox", "Inbox"), mailbox("important", "Important")},
        {mailbox("inbox", "Other server Inbox")}, source, destination));

    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items.front().sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK_FALSE(plan.items.front().sourceDestroy);
}

TEST_CASE("scoped move rejects stale membership while unscoped search move removes all residencies",
          "[app][mail-transfer][planning][move]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"archive", "important"}, {});
    const auto sourceMailboxes =
        std::vector{mailbox("inbox", "Inbox"), mailbox("archive", "Archive"),
                    mailbox("important", "Important")};

    const auto staleScopedMove =
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::optional<std::string>{"inbox"},
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Move},
                         {message.id}, {message}, sourceMailboxes,
                         {mailbox("archive", "Destination Archive")}, source, destination);
    CHECK(std::holds_alternative<QString>(staleScopedMove));

    const auto searchMove = requirePlan(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::nullopt,
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Move},
                         {message.id}, {message}, sourceMailboxes,
                         {mailbox("archive", "Destination Archive")}, source, destination));
    CHECK(searchMove.items.front().sourceRemoveMailboxIds == message.mailboxIds);
    CHECK(searchMove.items.front().sourceDestroy);
}

TEST_CASE("mail transfer planning rejects missing write rights instead of weakening semantics",
          "[app][mail-transfer][planning][rights]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox"});

    auto noAdd = mailbox("archive", "Archive");
    noAdd.myRights.mayAddItems = false;
    CHECK(std::holds_alternative<QString>(planMailTransfer(
        {.sourceAccountId = source.accountId,
         .sourceMailboxId = std::optional<std::string>{"inbox"},
         .destinationAccountId = destination.accountId,
         .destinationMailboxId = "archive",
         .operation = MailTransferOperation::Copy},
        {message.id}, {message}, {mailbox("inbox", "Inbox")}, {noAdd}, source, destination)));

    auto noRemove = mailbox("inbox", "Inbox");
    noRemove.myRights.mayRemoveItems = false;
    CHECK(std::holds_alternative<QString>(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::optional<std::string>{"inbox"},
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Move},
                         {message.id}, {message}, {noRemove}, {mailbox("archive", "Archive")},
                         source, destination)));

    auto noSeen = mailbox("archive", "Archive");
    noSeen.myRights.maySetSeen = false;
    CHECK(std::holds_alternative<QString>(planMailTransfer(
        {.sourceAccountId = source.accountId,
         .sourceMailboxId = std::optional<std::string>{"inbox"},
         .destinationAccountId = destination.accountId,
         .destinationMailboxId = "archive",
         .operation = MailTransferOperation::Copy},
        {message.id}, {message}, {mailbox("inbox", "Inbox")}, {noSeen}, source, destination)));

    auto noKeywords = mailbox("archive", "Archive");
    noKeywords.myRights.maySetKeywords = false;
    const auto flaggedOnly = email("email-2", {"inbox"}, {"$flagged"});
    CHECK(std::holds_alternative<QString>(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::optional<std::string>{"inbox"},
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Copy},
                         {flaggedOnly.id}, {flaggedOnly}, {mailbox("inbox", "Inbox")}, {noKeywords},
                         source, destination)));
}

TEST_CASE("exact redo cleanup removes only requested source membership and preserves later filing",
          "[app][mail-transfer][planning][redo]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox", "new-mailbox"}, {});
    const std::vector overrides{MailTransferSourceCleanupOverride{
        .emailId = message.id,
        .removeMailboxIds = {"inbox"},
    }};
    const auto plan = requirePlan(planMailTransfer(
        {.sourceAccountId = source.accountId,
         .sourceMailboxId = std::nullopt,
         .destinationAccountId = destination.accountId,
         .destinationMailboxId = "archive",
         .operation = MailTransferOperation::Move},
        {message.id}, {message}, {mailbox("inbox", "Inbox"), mailbox("new-mailbox", "New")},
        {mailbox("archive", "Archive")}, source, destination, overrides));

    REQUIRE(plan.items.size() == 1);
    CHECK(plan.items.front().sourceRemoveMailboxIds == std::vector<std::string>{"inbox"});
    CHECK_FALSE(plan.items.front().sourceDestroy);
}

TEST_CASE("exact redo cleanup becomes destroy only when requested memberships cover current source",
          "[app][mail-transfer][planning][redo]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox"}, {});
    const std::vector overrides{MailTransferSourceCleanupOverride{
        .emailId = message.id,
        .removeMailboxIds = {"inbox"},
    }};
    const auto plan = requirePlan(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::nullopt,
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Move},
                         {message.id}, {message}, {mailbox("inbox", "Inbox")},
                         {mailbox("archive", "Archive")}, source, destination, overrides));
    CHECK(plan.items.front().sourceDestroy);
}

TEST_CASE("exact redo cleanup rejects missing membership and unrelated override",
          "[app][mail-transfer][planning][redo]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox"}, {});

    const std::vector missing{MailTransferSourceCleanupOverride{
        .emailId = message.id,
        .removeMailboxIds = {"gone"},
    }};
    CHECK(std::holds_alternative<QString>(planMailTransfer(
        {.sourceAccountId = source.accountId,
         .sourceMailboxId = std::nullopt,
         .destinationAccountId = destination.accountId,
         .destinationMailboxId = "archive",
         .operation = MailTransferOperation::Move},
        {message.id}, {message}, {mailbox("inbox", "Inbox"), mailbox("gone", "Gone")},
        {mailbox("archive", "Archive")}, source, destination, missing)));

    const std::vector unrelated{
        MailTransferSourceCleanupOverride{.emailId = message.id, .removeMailboxIds = {"inbox"}},
        MailTransferSourceCleanupOverride{.emailId = "other-email", .removeMailboxIds = {"inbox"}},
    };
    CHECK(std::holds_alternative<QString>(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::nullopt,
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Move},
                         {message.id}, {message}, {mailbox("inbox", "Inbox")},
                         {mailbox("archive", "Archive")}, source, destination, unrelated)));
}

TEST_CASE("mail transfer planning deduplicates exact email ids and rejects same-account routing",
          "[app][mail-transfer][planning]")
{
    const auto source = account("source-local", "connection-a", "u1");
    const auto destination = account("destination-local", "connection-b", "u1");
    const auto message = email("email-1", {"inbox"}, {});
    const auto plan = requirePlan(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::optional<std::string>{"inbox"},
                          .destinationAccountId = destination.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Copy},
                         {message.id, message.id}, {message}, {mailbox("inbox", "Inbox")},
                         {mailbox("archive", "Archive")}, source, destination));
    CHECK(plan.items.size() == 1);

    CHECK(std::holds_alternative<QString>(
        planMailTransfer({.sourceAccountId = source.accountId,
                          .sourceMailboxId = std::optional<std::string>{"inbox"},
                          .destinationAccountId = source.accountId,
                          .destinationMailboxId = "archive",
                          .operation = MailTransferOperation::Copy},
                         {message.id}, {message}, {mailbox("inbox", "Inbox")},
                         {mailbox("archive", "Archive")}, source, source)));
}
