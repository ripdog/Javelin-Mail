#include "jmap/search/EmailSearch.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_CASE("search cache keys distinguish fields and sorting", "[jmap][search]")
{
    const auto textKey = javelin::jmap::search::cacheKey(
        {.text = "from:alice@example.com"},
        {.property = javelin::jmap::query::EmailListSortProperty::ReceivedAt,
         .direction = javelin::jmap::query::EmailListSortDirection::Descending});
    const auto fromKey = javelin::jmap::search::cacheKey(
        {.from = "alice@example.com"},
        {.property = javelin::jmap::query::EmailListSortProperty::ReceivedAt,
         .direction = javelin::jmap::query::EmailListSortDirection::Descending});
    const auto ascendingKey = javelin::jmap::search::cacheKey(
        {.text = "from:alice@example.com"},
        {.property = javelin::jmap::query::EmailListSortProperty::ReceivedAt,
         .direction = javelin::jmap::query::EmailListSortDirection::Ascending});

    CHECK(textKey != fromKey);
    CHECK(textKey != ascendingKey);
}

TEST_CASE("search cache keys normalize surrounding whitespace", "[jmap][search]")
{
    const auto first = javelin::jmap::search::cacheKey({.subject = "  quarterly report  "}, {});
    const auto second = javelin::jmap::search::cacheKey({.subject = "quarterly report"}, {});

    CHECK(first == second);
}

TEST_CASE("quick filters compose into a mailbox-scoped JMAP filter", "[jmap][search][quick-filter]")
{
    const auto filter = javelin::jmap::search::toEmailQueryFilter(
        {
            .inMailbox = "mbx-inbox",
            .unreadOnly = true,
            .starredOnly = true,
            .hasAttachmentOnly = true,
            .fromContactsOnly = true,
            .tags = {"work", "important"},
            .quickText = "alice",
            .quickTextSender = true,
            .quickTextRecipients = false,
            .quickTextSubject = true,
            .quickTextBody = false,
        },
        {.contactAddresses = {"alice@example.com"}});

    REQUIRE(filter.operatorName == std::optional<std::string>{"AND"});
    const auto hasCondition = [&filter](const auto& predicate)
    { return std::ranges::any_of(filter.conditions, predicate); };
    CHECK(hasCondition([](const auto& condition)
                       { return condition.inMailbox == std::optional<std::string>{"mbx-inbox"}; }));
    CHECK(hasCondition([](const auto& condition)
                       { return condition.notKeyword == std::optional<std::string>{"$seen"}; }));
    CHECK(hasCondition([](const auto& condition)
                       { return condition.hasKeyword == std::optional<std::string>{"$flagged"}; }));
    CHECK(hasCondition([](const auto& condition)
                       { return condition.hasAttachment == std::optional<bool>{true}; }));
    CHECK(hasCondition(
        [](const auto& condition)
        { return condition.from == std::optional<std::string>{"alice@example.com"}; }));
    CHECK(hasCondition(
        [](const auto& condition)
        {
            return condition.operatorName == std::optional<std::string>{"OR"} &&
                   std::ranges::any_of(
                       condition.conditions, [](const auto& nested)
                       { return nested.hasKeyword == std::optional<std::string>{"work"}; });
        }));
    CHECK(hasCondition(
        [](const auto& condition)
        {
            return condition.operatorName == std::optional<std::string>{"OR"} &&
                   std::ranges::any_of(
                       condition.conditions, [](const auto& nested)
                       { return nested.subject == std::optional<std::string>{"alice"}; });
        }));
}
