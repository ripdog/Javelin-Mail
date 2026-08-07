#include "jmap/sync/MailboxQueryDescriptor.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("mailbox query keys distinguish windows with different query semantics",
          "[jmap][sync][query-key]")
{
    const javelin::jmap::sync::MailboxQueryDescriptor base{
        .mailboxId = "mbx-inbox",
        .sortProperty = "receivedAt",
        .isAscending = false,
        .collapseThreads = true,
    };
    const auto baseKey = javelin::jmap::sync::mailboxQueryKey(base);

    auto otherMailbox = base;
    otherMailbox.mailboxId = "mbx-archive";
    CHECK(javelin::jmap::sync::mailboxQueryKey(otherMailbox) != baseKey);

    auto otherSort = base;
    otherSort.sortProperty = "sentAt";
    CHECK(javelin::jmap::sync::mailboxQueryKey(otherSort) != baseKey);

    auto ascending = base;
    ascending.isAscending = true;
    CHECK(javelin::jmap::sync::mailboxQueryKey(ascending) != baseKey);

    auto uncollapsed = base;
    uncollapsed.collapseThreads = false;
    CHECK(javelin::jmap::sync::mailboxQueryKey(uncollapsed) != baseKey);
}

TEST_CASE("anchored mailbox windows use the server position as their cache address",
          "[jmap][sync][query-key][pagination]")
{
    CHECK(javelin::jmap::sync::materializedMailboxWindowOffset(0, false, 412) == 0);
    CHECK(javelin::jmap::sync::materializedMailboxWindowOffset(0, true, 412) == 412);
}
