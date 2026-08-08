#include "jmap/query/QueryDiff.h"

#include <catch2/catch_test_macros.hpp>

namespace
{

    [[nodiscard]] javelin::jmap::cache::MailboxTreeItem mailbox(std::string id, std::string name,
                                                                const std::uint64_t sortOrder)
    {
        return javelin::jmap::cache::MailboxTreeItem{
            .id = std::move(id),
            .name = std::move(name),
            .parentId = std::nullopt,
            .role = std::nullopt,
            .sortOrder = sortOrder,
            .totalEmails = 0,
            .unreadEmails = 0,
            .totalThreads = 0,
            .unreadThreads = 0,
            .isSubscribed = true,
            .myRights = {},
            .hasChildren = false,
        };
    }

    [[nodiscard]] javelin::jmap::cache::MessageListItem
    message(std::string id, std::string threadId, std::string subject, std::string receivedAt)
    {
        return javelin::jmap::cache::MessageListItem{
            .emailId = std::move(id),
            .threadId = std::move(threadId),
            .subject = std::move(subject),
            .preview = std::nullopt,
            .receivedAt = std::move(receivedAt),
            .sentAt = std::nullopt,
            .hasAttachment = false,
            .isUnread = true,
            .isFlagged = false,
            .from = std::nullopt,
            .mailboxNames = {},
        };
    }

} // namespace

TEST_CASE("mailbox tree diff preserves selection and reports insert move and update changes",
          "[jmap][query]")
{
    const auto previous = std::vector{
        mailbox("mbx-inbox", "Inbox", 10),
        mailbox("mbx-archive", "Archive", 20),
    };
    auto renamedInbox = mailbox("mbx-inbox", "Inbox Renamed", 10);
    const auto current = std::vector{
        mailbox("mbx-archive", "Archive", 5),
        renamedInbox,
        mailbox("mbx-projects", "Projects", 30),
    };

    const auto refresh = javelin::jmap::query::diffMailboxTree(
        previous, current, javelin::jmap::query::MailboxSelectionKey{.mailboxId = "mbx-archive"});

    CHECK(refresh.selectionPreserved);
    REQUIRE(refresh.nextSelection.has_value());
    CHECK(refresh.nextSelection->mailboxId == "mbx-archive");
    CHECK_FALSE(refresh.changes.empty());
}

TEST_CASE("message list diff falls back to the first visible item when selection disappears",
          "[jmap][query]")
{
    const auto previous = std::vector{
        message("eml-1", "thread-1", "First", "2026-04-05T10:00:00Z"),
        message("eml-2", "thread-2", "Second", "2026-04-05T09:00:00Z"),
    };
    const auto current = std::vector{
        message("eml-3", "thread-3", "Newest", "2026-04-05T11:00:00Z"),
        message("eml-4", "thread-2", "Second updated", "2026-04-05T09:00:00Z"),
    };

    const auto refresh = javelin::jmap::query::diffMessageList(
        previous, current, javelin::jmap::query::MessageSelectionKey{.threadId = "thread-2"});

    CHECK(refresh.selectionPreserved);
    REQUIRE(refresh.nextSelection.has_value());
    CHECK(refresh.nextSelection->threadId == "thread-2");
    CHECK_FALSE(refresh.changes.empty());
}

TEST_CASE("message list diff reports tag presentation changes", "[jmap][query][tags]")
{
    auto previousMessage = message("eml-1", "thread-1", "First", "2026-04-05T10:00:00Z");
    auto currentMessage = previousMessage;
    currentMessage.tags.push_back(javelin::jmap::cache::MessageListTag{
        .keyword = "work",
        .displayName = QStringLiteral("Work"),
        .color = QStringLiteral("#123456"),
    });

    const auto refresh = javelin::jmap::query::diffMessageList(
        {previousMessage}, {currentMessage},
        javelin::jmap::query::MessageSelectionKey{.threadId = "thread-1"});

    REQUIRE(refresh.changes.size() == 1);
    CHECK(refresh.changes.front().kind == javelin::jmap::query::QueryRowChangeKind::Update);
}

TEST_CASE("selection fallback keeps the same row when a next item is available", "[jmap][query]")
{
    const auto fallback = javelin::jmap::query::selectionFallbackIndexAfterRemoval(1, 3);

    REQUIRE(fallback.has_value());
    CHECK(*fallback == 1);
}

TEST_CASE("selection fallback moves to the previous row when the removed row was last",
          "[jmap][query]")
{
    const auto fallback = javelin::jmap::query::selectionFallbackIndexAfterRemoval(2, 2);

    REQUIRE(fallback.has_value());
    CHECK(*fallback == 1);
}

TEST_CASE("selection fallback clears when the list becomes empty", "[jmap][query]")
{
    const auto fallback = javelin::jmap::query::selectionFallbackIndexAfterRemoval(0, 0);

    CHECK_FALSE(fallback.has_value());
}
