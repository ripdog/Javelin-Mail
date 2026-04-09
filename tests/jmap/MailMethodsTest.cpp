#include "jmap/api/MailMethods.h"
#include "FixtureReader.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("get requests serialize typed account, ids, and properties", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeGetRequest({
        .accountId = "u1",
        .ids = std::vector<std::string>{"mbx-inbox"},
        .idsReference = std::nullopt,
        .properties = std::vector<std::string>{"id", "name"},
    });

    REQUIRE(json.has_value());
    CHECK(*json == R"({"accountId":"u1","ids":["mbx-inbox"],"properties":["id","name"]})");
}

TEST_CASE("get requests serialize result references for chained ids", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeGetRequest({
        .accountId = "u1",
        .ids = std::nullopt,
        .idsReference =
            javelin::jmap::api::GetRequest::ResultReference{
                .resultOf = "mailbox-query",
                .name = "Email/query",
                .path = "/ids",
            },
        .properties = std::vector<std::string>{"threadId"},
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","#ids":{"resultOf":"mailbox-query","name":"Email/query","path":"/ids"},"properties":["threadId"]})");
}

TEST_CASE("changes requests serialize typed state-token inputs", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeChangesRequest({
        .accountId = "u1",
        .sinceState = "state-1",
        .maxChanges = 100,
    });

    REQUIRE(json.has_value());
    CHECK(*json == R"({"accountId":"u1","sinceState":"state-1","maxChanges":100})");
}

TEST_CASE("email query requests serialize mailbox-scoped sort windows", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryRequest({
        .accountId = "u1",
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = "mbx-inbox",
                .text = std::nullopt,
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .position = 0,
        .limit = 100,
        .collapseThreads = false,
        .calculateTotal = false,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","filter":{"inMailbox":"mbx-inbox"},"sort":[{"property":"receivedAt","isAscending":false}],"position":0,"limit":100,"collapseThreads":false,"calculateTotal":false})");
}

TEST_CASE("email query requests serialize text search filters", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryRequest({
        .accountId = "u1",
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = std::nullopt,
                .text = "quarterly report",
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .position = 0,
        .limit = 25,
        .collapseThreads = true,
        .calculateTotal = true,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","filter":{"text":"quarterly report"},"sort":[{"property":"receivedAt","isAscending":false}],"position":0,"limit":25,"collapseThreads":true,"calculateTotal":true})");
}

TEST_CASE("email queryChanges requests serialize incremental mailbox windows",
          "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailQueryChangesRequest({
        .accountId = "u1",
        .sinceQueryState = "query-state-1",
        .maxChanges = 50,
        .upToId = std::nullopt,
        .filter =
            javelin::jmap::api::EmailQueryFilter{
                .inMailbox = "mbx-inbox",
                .text = std::nullopt,
            },
        .sort =
            {
                javelin::jmap::api::EmailQuerySort{
                    .property = "receivedAt",
                    .isAscending = false,
                },
            },
        .collapseThreads = true,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","sinceQueryState":"query-state-1","maxChanges":50,"filter":{"inMailbox":"mbx-inbox"},"sort":[{"property":"receivedAt","isAscending":false}],"collapseThreads":true})");
}

TEST_CASE("mailbox get responses parse into typed mailbox entities", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseMailboxGetResponse(
        javelin::tests::loadFixture("jmap/method/mailbox_get_arguments.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "mailbox-state-1");
    REQUIRE(result.value->list.size() == 1);
    CHECK(result.value->list.front().id == "mbx-inbox");
    CHECK(result.value->list.front().role == std::optional<std::string>{"inbox"});
}

TEST_CASE("email get responses parse into typed email entities", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailGetResponse(
        javelin::tests::loadFixture("jmap/method/email_get_arguments.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "email-state-1");
    REQUIRE(result.value->list.size() == 1);
    CHECK(result.value->list.front().id == "eml-1");
    CHECK(result.value->list.front().threadId == "thr-123");
    CHECK(result.value->list.front().subject == std::optional<std::string>{"Quarterly update"});
}

TEST_CASE("thread get responses parse into typed thread entities", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseThreadGetResponse(
        R"({"accountId":"u1","state":"thread-state-1","list":[{"id":"thr-1","emailIds":["eml-1","eml-2"]}],"notFound":[]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "thread-state-1");
    REQUIRE(result.value->list.size() == 1);
    CHECK(result.value->list.front().id == "thr-1");
    CHECK(result.value->list.front().emailIds == std::vector<std::string>{"eml-1", "eml-2"});
}

TEST_CASE("changes responses parse created updated and destroyed ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseChangesResponse(
        javelin::tests::loadFixture("jmap/method/changes_arguments.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState == "state-1");
    CHECK(result.value->newState == "state-2");
    CHECK(result.value->hasMoreChanges);
    CHECK(result.value->created == std::vector<std::string>{"mbx-created"});
    CHECK(result.value->updated == std::vector<std::string>{"mbx-updated"});
    CHECK(result.value->destroyed == std::vector<std::string>{"mbx-destroyed"});
}

TEST_CASE("email changes responses parse typed incremental ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailChangesResponse(
        R"({"accountId":"u1","oldState":"state-1","newState":"state-2","hasMoreChanges":false,"created":["eml-created"],"updated":["eml-updated"],"destroyed":["eml-destroyed"]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState == "state-1");
    CHECK(result.value->newState == "state-2");
    CHECK_FALSE(result.value->hasMoreChanges);
    CHECK(result.value->created == std::vector<std::string>{"eml-created"});
    CHECK(result.value->updated == std::vector<std::string>{"eml-updated"});
    CHECK(result.value->destroyed == std::vector<std::string>{"eml-destroyed"});
}

TEST_CASE("email query responses parse ids and query metadata", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailQueryResponse(
        R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1","eml-2"],"total":2})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->queryState == "query-state-1");
    CHECK(result.value->canCalculateChanges);
    CHECK(result.value->ids == std::vector<std::string>{"eml-1", "eml-2"});
    CHECK(result.value->total == std::optional<std::uint64_t>{2});
}

TEST_CASE("email query responses ignore unknown server fields", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailQueryResponse(
        R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1","eml-2"],"total":2,"collapseThreads":false,"isQueryComplete":true})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->queryState == "query-state-1");
    CHECK(result.value->ids == std::vector<std::string>{"eml-1", "eml-2"});
    CHECK(result.value->total == std::optional<std::uint64_t>{2});
}

TEST_CASE("email queryChanges responses parse added and removed ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailQueryChangesResponse(
        R"({"accountId":"u1","oldQueryState":"query-state-1","newQueryState":"query-state-2","added":[{"id":"eml-3","index":0}],"removed":["eml-1"],"hasMoreChanges":false,"total":2})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldQueryState == "query-state-1");
    CHECK(result.value->newQueryState == "query-state-2");
    REQUIRE(result.value->added.size() == 1);
    CHECK(result.value->added.front().id == "eml-3");
    CHECK(result.value->added.front().index == 0);
    CHECK(result.value->removed == std::vector<std::string>{"eml-1"});
    CHECK_FALSE(result.value->hasMoreChanges);
    CHECK(result.value->total == std::optional<std::uint64_t>{2});
}

TEST_CASE("email content get requests serialize body fetch arguments", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailContentGetRequest({
        .accountId = "u1",
        .ids = {"eml-1"},
        .properties = {"id", "textBody", "htmlBody", "attachments", "bodyValues"},
        .bodyProperties = {"partId", "blobId", "size", "name", "type", "charset", "disposition",
                           "cid"},
        .fetchTextBodyValues = true,
        .fetchHTMLBodyValues = true,
        .fetchAllBodyValues = false,
        .maxBodyValueBytes = 131072,
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","ids":["eml-1"],"properties":["id","textBody","htmlBody","attachments","bodyValues"],"bodyProperties":["partId","blobId","size","name","type","charset","disposition","cid"],"fetchTextBodyValues":true,"fetchHTMLBodyValues":true,"fetchAllBodyValues":false,"maxBodyValueBytes":131072})");
}

TEST_CASE("email content get responses parse typed body sections and values",
          "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailContentGetResponse(
        R"({"accountId":"u1","state":"email-state-1","list":[{"id":"eml-1","textBody":[{"partId":"1","blobId":"blob-text","size":24,"type":"text/plain","charset":"utf-8"}],"htmlBody":[{"partId":"2","blobId":"blob-html","size":48,"type":"text/html","charset":"utf-8","cid":"inline-1"}],"attachments":[{"partId":"3","blobId":"blob-pdf","size":5120,"name":"report.pdf","type":"application/pdf","disposition":"attachment"}],"bodyValues":{"1":{"isEncodingProblem":false,"isTruncated":false,"value":"Plain text body"},"2":{"isEncodingProblem":false,"isTruncated":true,"value":"<p>HTML body</p>"}}}],"notFound":[]})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->state == "email-state-1");
    REQUIRE(result.value->list.size() == 1);
    const auto& content = result.value->list.front();
    CHECK(content.id == "eml-1");
    REQUIRE(content.textBody.size() == 1);
    CHECK(content.textBody.front().partId == std::optional<std::string>{"1"});
    CHECK(content.textBody.front().type == "text/plain");
    REQUIRE(content.htmlBody.size() == 1);
    CHECK(content.htmlBody.front().cid == std::optional<std::string>{"inline-1"});
    REQUIRE(content.attachments.size() == 1);
    CHECK(content.attachments.front().name == std::optional<std::string>{"report.pdf"});
    REQUIRE(content.bodyValues.contains("1"));
    CHECK(content.bodyValues.at("1").value == "Plain text body");
    REQUIRE(content.bodyValues.contains("2"));
    CHECK(content.bodyValues.at("2").isTruncated);
}

TEST_CASE("email set requests serialize typed mailbox and keyword updates", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeEmailSetRequest({
        .accountId = "u1",
        .update =
            {
                {"eml-1",
                 javelin::jmap::api::EmailSetUpdate{
                     .mailboxIds = {{"mbx-archive", true}},
                     .keywords = {{"$seen", true}},
                 }},
            },
    });

    REQUIRE(json.has_value());
    CHECK(
        *json ==
        R"({"accountId":"u1","update":{"eml-1":{"mailboxIds":{"mbx-archive":true},"keywords":{"$seen":true}}}})");
}

TEST_CASE("email set responses parse updated and failed ids", "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailSetResponse(
        R"({"accountId":"u1","oldState":"old-1","newState":"new-1","updated":{"eml-1":null},"notUpdated":{"eml-2":{"type":"forbidden"}}})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState == "old-1");
    CHECK(result.value->newState == "new-1");
    CHECK(result.value->updated == std::vector<std::string>{"eml-1"});
    CHECK(result.value->notUpdated == std::vector<std::string>{"eml-2"});
}

TEST_CASE("email set responses accept nullable oldState and null updated maps",
          "[jmap][method][mail]")
{
    const auto result = javelin::jmap::api::parseEmailSetResponse(
        R"({"accountId":"u1","oldState":null,"newState":"new-1","updated":null,"notUpdated":null})");

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->accountId == "u1");
    CHECK(result.value->oldState.empty());
    CHECK(result.value->newState == "new-1");
    CHECK(result.value->updated.empty());
    CHECK(result.value->notUpdated.empty());
}
