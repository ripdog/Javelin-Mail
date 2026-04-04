#include "jmap/query/MessageGrouping.h"

#include <QDate>

#include <catch2/catch_test_macros.hpp>

namespace
{

    [[nodiscard]] javelin::jmap::cache::MessageListItem message(std::string id,
                                                                std::string receivedAt)
    {
        return javelin::jmap::cache::MessageListItem{
            .emailId = std::move(id),
            .threadId = "thread",
            .subject = std::nullopt,
            .preview = std::nullopt,
            .receivedAt = std::move(receivedAt),
            .sentAt = std::nullopt,
            .hasAttachment = false,
            .isUnread = true,
            .isFlagged = false,
            .from = std::nullopt,
        };
    }

} // namespace

TEST_CASE(
    "message grouping keeps recent messages in day buckets and older messages in month buckets",
    "[jmap][query]")
{
    const auto items = std::vector{
        message("eml-1", "2026-04-05T10:00:00Z"), message("eml-2", "2026-04-05T08:00:00Z"),
        message("eml-3", "2026-04-03T12:00:00Z"), message("eml-4", "2026-03-20T09:00:00Z"),
        message("eml-5", "2026-03-01T09:00:00Z"),
    };

    const auto groups = javelin::jmap::query::groupMessagesByTime(items, QDate{2026, 4, 5});
    REQUIRE(groups.size() == 3);
    CHECK(groups.at(0).id == "day:2026-04-05");
    CHECK(groups.at(0).count == 2);
    CHECK(groups.at(1).id == "day:2026-04-03");
    CHECK(groups.at(1).count == 1);
    CHECK(groups.at(2).id == "month:2026-03");
    CHECK(groups.at(2).count == 2);
}

TEST_CASE("message grouping remains stable for a paged window cut from a larger list",
          "[jmap][query]")
{
    const auto page = std::vector{
        message("eml-2", "2026-04-05T08:00:00Z"),
        message("eml-3", "2026-04-03T12:00:00Z"),
        message("eml-4", "2026-03-20T09:00:00Z"),
    };

    const auto groups = javelin::jmap::query::groupMessagesByTime(page, QDate{2026, 4, 5});
    REQUIRE(groups.size() == 3);
    CHECK(groups.at(0).id == "day:2026-04-05");
    CHECK(groups.at(0).startIndex == 0);
    CHECK(groups.at(1).id == "day:2026-04-03");
    CHECK(groups.at(1).startIndex == 1);
    CHECK(groups.at(2).id == "month:2026-03");
    CHECK(groups.at(2).startIndex == 2);
}
