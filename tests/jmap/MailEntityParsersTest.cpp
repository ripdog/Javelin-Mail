#include "jmap/domain/MailEntityParsers.h"
#include "FixtureReader.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("mailbox fixtures parse into typed mailbox entities", "[jmap][domain]")
{
    const auto result = javelin::jmap::domain::parseMailbox(
        javelin::tests::loadFixture("jmap/entities/mailbox.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->id == "mbx-inbox");
    CHECK(result.value->role == "inbox");
    CHECK(result.value->unreadEmails == 7U);
    CHECK(result.value->myRights.mayReadItems);
    CHECK_FALSE(result.value->myRights.mayDelete);
}

TEST_CASE("mailbox parser ignores unknown server fields", "[jmap][domain]")
{
    const auto result = javelin::jmap::domain::parseMailbox(
        R"({"id":"mbx-inbox","name":"Inbox","parentId":null,"role":"inbox","sortOrder":10,"totalEmails":125,"unreadEmails":7,"totalThreads":98,"unreadThreads":5,"isSubscribed":true,"didFoldersCheck":1,"hidden":0,"autoPurge":false,"learnAsSpam":false,"autoLearn":false,"myRights":{"mayReadItems":true,"mayAddItems":true,"mayRemoveItems":true,"maySetSeen":true,"maySetKeywords":true,"mayCreateChild":false,"mayRename":false,"mayDelete":false,"maySubmit":true}})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->id == "mbx-inbox");
    CHECK(result.value->role == "inbox");
}

TEST_CASE("thread fixtures parse into typed thread entities", "[jmap][domain]")
{
    const auto result = javelin::jmap::domain::parseThread(
        javelin::tests::loadFixture("jmap/entities/thread.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->id == "thr-123");
    REQUIRE(result.value->emailIds.size() == 3);
    CHECK(result.value->emailIds.front() == "eml-1");
}

TEST_CASE("email fixtures parse into typed email entities", "[jmap][domain]")
{
    const auto result =
        javelin::jmap::domain::parseEmail(javelin::tests::loadFixture("jmap/entities/email.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->id == "eml-1");
    CHECK(result.value->threadId == "thr-123");
    CHECK(result.value->hasAttachment);
    CHECK(result.value->mailboxIds.size() == 1);
    CHECK(result.value->mailboxIds.front() == "mbx-inbox");
    REQUIRE(result.value->keywords.size() == 2);
    CHECK(result.value->subject == "Quarterly update");
    REQUIRE(result.value->from.size() == 1);
    CHECK(result.value->from.front().email == "alice@example.com");
    REQUIRE(result.value->replyTo.size() == 1);
    CHECK(result.value->replyTo.front().email == "support@example.com");
}

TEST_CASE("email parser ignores unknown server fields and missing optional arrays",
          "[jmap][domain]")
{
    const auto result = javelin::jmap::domain::parseEmail(
        R"({"id":"eml-fastmail","blobId":"blob-fastmail","threadId":"thr-fastmail","mailboxIds":{"mbx-inbox":true},"keywords":{"$seen":true},"size":2048,"receivedAt":"2026-04-05T11:22:33Z","hasAttachment":false,"subject":"Inbox update","from":[{"name":"Fastmail","email":"no-reply@fastmail.com"}],"preview":"Preview text","sender":null,"bodyValues":{},"attachments":[]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->id == "eml-fastmail");
    CHECK(result.value->blobId == "blob-fastmail");
    CHECK(result.value->threadId == "thr-fastmail");
    CHECK(result.value->mailboxIds == std::vector<std::string>{"mbx-inbox"});
    CHECK(result.value->keywords == std::vector<std::string>{"$seen"});
    CHECK(result.value->from.size() == 1);
    CHECK(result.value->to.empty());
    CHECK(result.value->cc.empty());
    CHECK(result.value->bcc.empty());
    CHECK(result.value->replyTo.empty());
}

TEST_CASE("identity fixtures parse into typed identity entities", "[jmap][domain]")
{
    const auto result = javelin::jmap::domain::parseIdentity(
        javelin::tests::loadFixture("jmap/entities/identity.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->id == "ident-1");
    CHECK(result.value->email == "alice@example.com");
    REQUIRE(result.value->bcc.size() == 1);
    CHECK(result.value->bcc.front().email == "audit@example.com");
    CHECK(result.value->htmlSignature == "<p>Regards,<br>Alice</p>");
    CHECK_FALSE(result.value->mayDelete);
}

TEST_CASE("identity parser accepts nullable reply-to and bcc lists", "[jmap][domain]")
{
    const auto result = javelin::jmap::domain::parseIdentity(
        R"({"id":"b","name":"System administrator","email":"admin@mail.example.com","replyTo":null,"bcc":null,"textSignature":"","htmlSignature":"","mayDelete":true})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->replyTo.empty());
    CHECK(result.value->bcc.empty());
    CHECK(result.value->mayDelete);
}
