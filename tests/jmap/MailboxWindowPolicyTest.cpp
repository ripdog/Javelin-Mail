#include "jmap/sync/MailboxWindowPolicy.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    [[nodiscard]] javelin::jmap::sync::MailboxWindowAvailability availability()
    {
        return {
            .cachedRepresentativeCount = 100,
            .serverRepresentativeCount = 250,
            .offset = 0,
            .limit = 100,
            .sort = {},
            .forceRefresh = false,
        };
    }
} // namespace

TEST_CASE("canonical mailbox windows use cached contiguous rows", "[jmap][sync][window]")
{
    auto value = availability();
    CHECK(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));

    value.offset = 100;
    CHECK_FALSE(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));

    value.cachedRepresentativeCount = 200;
    CHECK(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));
}

TEST_CASE("noncanonical mailbox sorting requires a complete local mailbox", "[jmap][sync][window]")
{
    auto value = availability();
    value.sort.property = javelin::jmap::query::EmailListSortProperty::Subject;
    CHECK_FALSE(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));

    value.cachedRepresentativeCount = value.serverRepresentativeCount;
    CHECK(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));
}

TEST_CASE("empty and out-of-range windows need no network acquisition", "[jmap][sync][window]")
{
    auto value = availability();
    value.serverRepresentativeCount = 0;
    value.cachedRepresentativeCount = 0;
    CHECK(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));

    value.serverRepresentativeCount = 25;
    value.offset = 25;
    CHECK(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));
    CHECK(javelin::jmap::sync::cachedMailboxWindowSize(value) == 0);
}

TEST_CASE("forced mailbox refresh bypasses a complete cache", "[jmap][sync][window]")
{
    auto value = availability();
    value.cachedRepresentativeCount = value.serverRepresentativeCount;
    value.forceRefresh = true;
    CHECK_FALSE(javelin::jmap::sync::cacheSatisfiesMailboxWindow(value));
}

TEST_CASE("cached mailbox window size is clipped to available rows", "[jmap][sync][window]")
{
    auto value = availability();
    value.cachedRepresentativeCount = 125;
    value.offset = 100;
    CHECK(javelin::jmap::sync::cachedMailboxWindowSize(value) == 25);

    value.offset = 125;
    CHECK(javelin::jmap::sync::cachedMailboxWindowSize(value) == 0);
}
