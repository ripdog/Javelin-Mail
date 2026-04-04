#include "jmap/api/MethodEnvelope.h"
#include "FixtureReader.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("request envelopes parse typed invocation tuples from fixtures", "[jmap][method]")
{
    const auto result = javelin::jmap::api::parseRequestEnvelope(
        javelin::tests::loadFixture("jmap/method/request.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->usingCapabilities.size() == 2);
    REQUIRE(result.value->methodCalls.size() == 2);
    CHECK(result.value->methodCalls.front().name == "Mailbox/get");
    CHECK(result.value->methodCalls.front().callId == "c1");
    REQUIRE(result.value->createdIds.has_value());
    CHECK(result.value->createdIds->at("draft-1") == "eml-draft-server-id");
}

TEST_CASE("response envelopes parse typed invocation tuples from fixtures", "[jmap][method]")
{
    const auto result = javelin::jmap::api::parseResponseEnvelope(
        javelin::tests::loadFixture("jmap/method/response.json"));

    REQUIRE(result.ok());
    REQUIRE(result.value.has_value());
    CHECK(result.value->sessionState == "session-state-2");
    REQUIRE(result.value->methodResponses.size() == 2);
    CHECK(result.value->methodResponses.back().name == "Email/get");
    CHECK(result.value->methodResponses.back().callId == "c2");
}

TEST_CASE("request envelopes serialize and round-trip", "[jmap][method]")
{
    const auto parsed = javelin::jmap::api::parseRequestEnvelope(
        javelin::tests::loadFixture("jmap/method/request.json"));
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value.has_value());

    const auto serialized = javelin::jmap::api::serializeRequestEnvelope(*parsed.value);
    REQUIRE(serialized.has_value());

    const auto reparsed = javelin::jmap::api::parseRequestEnvelope(*serialized);
    REQUIRE(reparsed.ok());
    REQUIRE(reparsed.value.has_value());
    CHECK(reparsed.value->methodCalls.size() == parsed.value->methodCalls.size());
    CHECK(reparsed.value->methodCalls.front().arguments ==
          parsed.value->methodCalls.front().arguments);
}
