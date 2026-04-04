#include "jmap/api/MailMethods.h"
#include "FixtureReader.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("get requests serialize typed account, ids, and properties", "[jmap][method][mail]")
{
    const auto json = javelin::jmap::api::serializeGetRequest({
        .accountId = "u1",
        .ids = std::vector<std::string>{"mbx-inbox"},
        .properties = std::vector<std::string>{"id", "name"},
    });

    REQUIRE(json.has_value());
    CHECK(*json == R"({"accountId":"u1","ids":["mbx-inbox"],"properties":["id","name"]})");
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
