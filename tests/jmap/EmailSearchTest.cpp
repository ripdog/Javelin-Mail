#include "jmap/search/EmailSearch.h"

#include <catch2/catch_test_macros.hpp>

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
