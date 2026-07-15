#include "jmap/cache/SearchResultReconciler.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    [[nodiscard]] javelin::jmap::cache::MessageListItem item(std::string emailId,
                                                             std::string threadId)
    {
        return {.emailId = std::move(emailId),
                .threadId = std::move(threadId),
                .receivedAt = "2026-07-15T00:00:00Z"};
    }
} // namespace

TEST_CASE("server search results merge without reordering existing matches", "[jmap][search]")
{
    const std::vector current{item("local-a", "thread-a"), item("local-b", "thread-b")};
    const std::vector server{item("server-c", "thread-c"), item("server-a", "thread-a")};

    const auto merged = javelin::jmap::cache::reconcileServerSearchResults(current, server, {});

    REQUIRE(merged.items.size() == 2);
    CHECK(merged.items[0].emailId == "server-a");
    CHECK(merged.items[1].emailId == "server-c");
    CHECK(merged.retainedLocalEmailIds.empty());
}

TEST_CASE("selected local-only search result survives server reconciliation", "[jmap][search]")
{
    const std::vector current{item("local-a", "thread-a"), item("local-b", "thread-b")};
    const std::vector server{item("server-c", "thread-c")};

    const auto merged = javelin::jmap::cache::reconcileServerSearchResults(
        current, server, std::string_view{"local-b"});

    REQUIRE(merged.items.size() == 2);
    CHECK(merged.items[0].emailId == "local-b");
    CHECK(merged.items[1].emailId == "server-c");
    CHECK(merged.retainedLocalEmailIds.contains("local-b"));
}
