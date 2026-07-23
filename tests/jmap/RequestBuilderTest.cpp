#include "jmap/api/RequestBuilder.h"
#include "jmap/api/ResponseReader.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("request builder emits multi-call envelopes with preserved call ids",
          "[jmap][api][builder]")
{
    javelin::jmap::api::RequestBuilder builder;
    builder.useCore().useMail();
    const auto first = builder.call(
        javelin::jmap::api::MethodRequest<javelin::jmap::api::EmailQueryResponse>{
            .name = "Email/query",
            .arguments = R"({"accountId":"u1"})",
        },
        "mailbox-query");
    const auto second =
        builder.call(javelin::jmap::api::MethodRequest<javelin::jmap::api::EmailGetResponse>{
            .name = "Email/get",
            .arguments = R"({"accountId":"u1","ids":["eml-1"]})",
        });

    const auto envelope = builder.build();

    REQUIRE(envelope.usingCapabilities.size() == 2);
    CHECK(envelope.usingCapabilities.front() == "urn:ietf:params:jmap:core");
    CHECK(envelope.usingCapabilities.back() == "urn:ietf:params:jmap:mail");
    REQUIRE(envelope.methodCalls.size() == 2);
    CHECK(first.callId == "mailbox-query");
    CHECK(envelope.methodCalls.front().callId == "mailbox-query");
    CHECK(second.callId == "call-1");
    CHECK(envelope.methodCalls.back().callId == "call-1");
}

TEST_CASE("response reader resolves typed responses by call handle", "[jmap][api][builder]")
{
    const javelin::jmap::api::ResponseEnvelope envelope{
        .methodResponses =
            {
                javelin::jmap::api::MethodInvocation{
                    .name = "Email/query",
                    .arguments =
                        R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                    .callId = "mailbox-query",
                },
            },
        .createdIds = std::nullopt,
        .sessionState = "session-state-1",
    };

    const javelin::jmap::api::ResponseReader reader{envelope};
    const auto result =
        reader.require(javelin::jmap::api::CallHandle<javelin::jmap::api::EmailQueryResponse>{
            .callId = "mailbox-query"});

    REQUIRE(std::holds_alternative<javelin::jmap::api::EmailQueryResponse>(result));
    const auto& response = std::get<javelin::jmap::api::EmailQueryResponse>(result);
    CHECK(response.queryState == "query-state-1");
    CHECK(response.ids == std::vector<std::string>{"eml-1"});
}

TEST_CASE("response reader surfaces method errors by call handle", "[jmap][api][builder]")
{
    const javelin::jmap::api::ResponseEnvelope envelope{
        .methodResponses =
            {
                javelin::jmap::api::MethodInvocation{
                    .name = "error",
                    .arguments =
                        R"({"type":"cannotCalculateChanges","description":"delta unavailable"})",
                    .callId = "mailbox-query-changes",
                },
            },
        .createdIds = std::nullopt,
        .sessionState = "session-state-1",
    };

    const javelin::jmap::api::ResponseReader reader{envelope};
    const auto result = reader.require(
        javelin::jmap::api::CallHandle<javelin::jmap::api::EmailQueryChangesResponse>{
            .callId = "mailbox-query-changes"});

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseReaderError>(result));
    const auto& error = std::get<javelin::jmap::api::ResponseReaderError>(result);
    CHECK(error.code == javelin::jmap::api::ResponseReaderErrorCode::MethodError);
    REQUIRE(error.methodError.has_value());
    CHECK(error.methodError->type == "cannotCalculateChanges");
    CHECK(error.methodError->description == std::optional<std::string>{"delta unavailable"});
}
