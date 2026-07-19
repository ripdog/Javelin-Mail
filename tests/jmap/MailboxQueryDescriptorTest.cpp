#include "jmap/sync/MailboxQueryDescriptor.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("mailbox query key is canonical and explicit", "[jmap][sync][query-key]")
{
    const auto key = javelin::jmap::sync::mailboxQueryKey({
        .mailboxId = "mbx-inbox",
        .sortProperty = "receivedAt",
        .isAscending = false,
        .collapseThreads = true,
    });

    CHECK(key == "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true");
}

TEST_CASE("anchored mailbox windows use the server position as their cache address",
          "[jmap][sync][query-key][pagination]")
{
    CHECK(javelin::jmap::sync::materializedMailboxWindowOffset(0, false, 412) == 0);
    CHECK(javelin::jmap::sync::materializedMailboxWindowOffset(0, true, 412) == 412);
}
