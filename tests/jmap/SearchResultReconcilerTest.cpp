#include "jmap/cache/SearchResultReconciler.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    [[nodiscard]] javelin::jmap::cache::MessageListItem item(std::string emailId,
                                                             std::string threadId)
    {
        return {.emailId = std::move(emailId),
                .threadId = std::move(threadId),
                .subject = std::nullopt,
                .preview = std::nullopt,
                .receivedAt = "2026-07-15T00:00:00Z",
                .sentAt = std::nullopt,
                .threadMessageCount = 1,
                .hasAttachment = false,
                .isUnread = false,
                .isFlagged = false,
                .from = std::nullopt};
    }
} // namespace

TEST_CASE("server search ordering drives reconciled results", "[jmap][search]")
{
    const std::vector current{item("local-a", "thread-a"), item("local-b", "thread-b")};
    const std::vector server{item("server-c", "thread-c"), item("server-a", "thread-a")};

    const auto merged = javelin::jmap::cache::reconcileServerSearchResults(current, server, {});

    REQUIRE(merged.items.size() == 2);
    CHECK(merged.items[0].emailId == "server-c");
    CHECK(merged.items[1].emailId == "server-a");
    CHECK(merged.retainedLocalEmailIds.empty());
}

TEST_CASE("selected local-only search result survives server reconciliation", "[jmap][search]")
{
    const std::vector current{item("local-a", "thread-a"), item("local-b", "thread-b")};
    const std::vector server{item("server-c", "thread-c")};

    const auto merged = javelin::jmap::cache::reconcileServerSearchResults(
        current, server, std::string_view{"local-b"});

    REQUIRE(merged.items.size() == 2);
    CHECK(merged.items[0].emailId == "server-c");
    CHECK(merged.items[1].emailId == "local-b");
    CHECK(merged.retainedLocalEmailIds.contains("local-b"));
}

TEST_CASE("selected local representative keeps its identity at the server thread position",
          "[jmap][search]")
{
    const std::vector current{item("selected-a", "thread-a"), item("local-b", "thread-b")};
    const std::vector server{item("server-c", "thread-c"), item("server-a", "thread-a")};

    const auto merged = javelin::jmap::cache::reconcileServerSearchResults(
        current, server, std::string_view{"selected-a"});

    REQUIRE(merged.items.size() == 2);
    CHECK(merged.items[0].emailId == "server-c");
    CHECK(merged.items[1].emailId == "selected-a");
    CHECK(merged.retainedLocalEmailIds.contains("selected-a"));
}
