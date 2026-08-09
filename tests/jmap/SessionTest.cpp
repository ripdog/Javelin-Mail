#include "jmap/api/Session.h"
#include "FixtureReader.h"
#include "jmap/api/SessionParser.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

TEST_CASE("session capability validation succeeds when required capabilities are present", "[jmap]")
{
    const auto sessionJson = javelin::tests::loadFixture("jmap/session/basic_session.json");
    const auto result = javelin::jmap::api::parseSession(
        sessionJson, javelin::jmap::api::RequiredCapabilities{.mail = true, .submission = true});

    CHECK(result.ok());
    REQUIRE(result.session.has_value());
    CHECK(result.session->username == "alice@example.com");
    CHECK(result.session->capabilities.core);
    CHECK(result.session->capabilities.mail);
    CHECK(result.session->capabilities.submission);
    REQUIRE(result.session->capabilities.coreDetails.has_value());
    CHECK(result.session->capabilities.coreDetails->maxConcurrentRequests == 8U);
    CHECK(result.session->primaryAccounts.mailAccountId == "u1");
    CHECK(result.session->accounts.at("u1").accountCapabilities.mail);
    REQUIRE(result.session->accounts.at("u1").accountCapabilities.submission.has_value());
}

TEST_CASE("session parser exposes delayed send limits", "[jmap][session][submission]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.com","apiUrl":"https://mail.example.com/jmap/api","downloadUrl":"https://mail.example.com/download/{accountId}/{blobId}/{name}","uploadUrl":"https://mail.example.com/upload/{accountId}","state":"s1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:submission":{}},"accounts":{"u1":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:submission":{"maxDelayedSend":2592000,"submissionExtensions":{"FUTURERELEASE":[]}}}}},"primaryAccounts":{"urn:ietf:params:jmap:submission":"u1"}})",
        {.submission = true});

    REQUIRE(result.ok());
    REQUIRE(result.session.has_value());
    const auto& capability = result.session->accounts.at("u1").accountCapabilities.submission;
    REQUIRE(capability.has_value());
    CHECK(capability->maxDelayedSend == 2592000U);
}

TEST_CASE("session parser ignores unknown server fields", "[jmap]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.com","apiUrl":"https://mail.example.com/jmap/api","downloadUrl":"https://mail.example.com/jmap/download/{accountId}/{blobId}/{name}?type={type}","uploadUrl":"https://mail.example.com/jmap/upload/{accountId}","eventSourceUrl":"https://mail.example.com/jmap/event/","state":"session-state-1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:mail":{"maxMailboxesPerEmail":1000}},"accounts":{"u1":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:mail":{}},"unexpectedAccountField":true}},"primaryAccounts":{"urn:ietf:params:jmap:mail":"u1"},"unexpectedTopLevelField":"ignored"})",
        javelin::jmap::api::RequiredCapabilities{.mail = true, .submission = false});

    REQUIRE(result.ok());
    REQUIRE(result.session.has_value());
    CHECK(result.session->username == "alice@example.com");
    CHECK(result.session->primaryAccounts.mailAccountId == "u1");
    CHECK(result.session->accounts.contains("u1"));
}

TEST_CASE("session parser exposes websocket push capability", "[jmap]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.com","apiUrl":"https://mail.example.com/jmap/api","downloadUrl":"https://mail.example.com/download/{accountId}/{blobId}/{name}","uploadUrl":"https://mail.example.com/upload/{accountId}","eventSourceUrl":"https://mail.example.com/events","state":"s1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:mail":{},"urn:ietf:params:jmap:websocket":{"url":"wss://mail.example.com/jmap/ws","supportsPush":true}},"accounts":{"u1":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:mail":{}}}},"primaryAccounts":{"urn:ietf:params:jmap:mail":"u1"}})",
        javelin::jmap::api::RequiredCapabilities{.mail = true});

    REQUIRE(result.ok());
    REQUIRE(result.session->capabilities.websocket.has_value());
    CHECK(result.session->capabilities.websocket->url == "wss://mail.example.com/jmap/ws");
    CHECK(result.session->capabilities.websocket->supportsPush);
}

TEST_CASE("session parser rejects incomplete mandatory core request limits", "[jmap][session]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.com","apiUrl":"https://mail.example.com/jmap/api","downloadUrl":"https://mail.example.com/download/{accountId}/{blobId}/{name}","uploadUrl":"https://mail.example.com/upload/{accountId}","state":"s1","capabilities":{"urn:ietf:params:jmap:core":{"maxCallsInRequest":16}},"accounts":{"u1":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{}}},"primaryAccounts":{}})",
        {});

    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error.has_value());
    CHECK_THAT(result.error->capabilityErrors,
               Catch::Matchers::VectorContains(
                   javelin::jmap::api::CapabilityError::InvalidCoreCapability));
}

TEST_CASE("session parser exposes contacts capability metadata", "[jmap][contacts]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.com","apiUrl":"https://example.test/jmap","downloadUrl":"https://example.test/download/{accountId}/{blobId}/{name}","uploadUrl":"https://example.test/upload/{accountId}","state":"1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:mail":{},"urn:ietf:params:jmap:contacts":{}},"accounts":{"u1":{"name":"Personal","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:mail":{},"urn:ietf:params:jmap:contacts":{"maxAddressBooksPerCard":4,"mayCreateAddressBook":true}}}},"primaryAccounts":{"urn:ietf:params:jmap:mail":"u1","urn:ietf:params:jmap:contacts":"u1"}})",
        {.mail = true});

    REQUIRE(result.ok());
    REQUIRE(result.session.has_value());
    CHECK(result.session->capabilities.contacts);
    CHECK(result.session->primaryAccounts.contactsAccountId == "u1");
    const auto& capability = result.session->accounts.at("u1").accountCapabilities.contacts;
    REQUIRE(capability.has_value());
    CHECK(capability->maxAddressBooksPerCard == 4);
    CHECK(capability->mayCreateAddressBook);
}

TEST_CASE("session parser exposes strict draft-26 calendar capability metadata", "[jmap][calendar]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.test","apiUrl":"https://example.test/jmap","downloadUrl":"https://example.test/download/{accountId}/{blobId}/{name}","uploadUrl":"https://example.test/upload/{accountId}","state":"1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:calendars":{}},"accounts":{"a1":{"name":"Calendar","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:calendars":{"maxCalendarsPerEvent":4,"minDateTime":"1900-01-01T00:00:00Z","maxDateTime":"2100-01-01T00:00:00Z","maxExpandedQueryDuration":"P1Y","maxParticipantsPerEvent":100,"mayCreateCalendar":false}}}},"primaryAccounts":{"urn:ietf:params:jmap:calendars":"a1"}})",
        {.calendars = true});

    REQUIRE(result.ok());
    REQUIRE(result.session->capabilities.calendars);
    CHECK(result.session->primaryAccounts.calendarsAccountId == "a1");
    const auto& capability = result.session->accounts.at("a1").accountCapabilities.calendars;
    REQUIRE(capability.has_value());
    CHECK(capability->maxCalendarsPerEvent == 4);
    CHECK(capability->maxExpandedQueryDuration == "P1Y");
}

TEST_CASE("session parser accepts the Stalwart draft-26 calendar capability shape",
          "[jmap][calendar][stalwart]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"calendar@example.test","apiUrl":"https://mail.example.test/jmap/","downloadUrl":"https://mail.example.test/jmap/download/{accountId}/{blobId}/{name}","uploadUrl":"https://mail.example.test/jmap/upload/{accountId}","state":"session-1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:calendars":{}},"accounts":{"c":{"name":"calendar@example.test","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:calendars":{"minDateTime":"0001-01-01T00:00:00Z","maxDateTime":"65534-12-31T23:59:59Z","maxExpandedQueryDuration":"P52W1D","maxParticipantsPerEvent":20,"mayCreateCalendar":true}}}},"primaryAccounts":{"urn:ietf:params:jmap:calendars":"c"}})",
        {.calendars = true});

    REQUIRE(result.ok());
    REQUIRE(result.session->accounts.at("c").accountCapabilities.calendars.has_value());
    const auto& calendars = *result.session->accounts.at("c").accountCapabilities.calendars;
    CHECK(calendars.minDateTime == "0001-01-01T00:00:00Z");
    CHECK(calendars.maxDateTime == "65534-12-31T23:59:59Z");
    CHECK(calendars.maxExpandedQueryDuration == "P52W1D");
    CHECK(calendars.mayCreateCalendar);
}

TEST_CASE("session parser rejects malformed required calendar account capability",
          "[jmap][calendar]")
{
    const auto result = javelin::jmap::api::parseSession(
        R"({"username":"alice@example.test","apiUrl":"https://example.test/jmap","downloadUrl":"https://example.test/download/{accountId}/{blobId}/{name}","uploadUrl":"https://example.test/upload/{accountId}","state":"1","capabilities":{"urn:ietf:params:jmap:core":{"maxSizeRequest":1000000,"maxConcurrentRequests":8,"maxCallsInRequest":16,"maxObjectsInGet":500,"maxObjectsInSet":500},"urn:ietf:params:jmap:calendars":{}},"accounts":{"a1":{"name":"Calendar","isPersonal":true,"isReadOnly":false,"accountCapabilities":{"urn:ietf:params:jmap:calendars":{"maxExpandedQueryDuration":"P1Y","mayCreateCalendar":false}}}},"primaryAccounts":{"urn:ietf:params:jmap:calendars":"a1"}})",
        {.calendars = true});

    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error.has_value());
    CHECK_THAT(result.error->capabilityErrors,
               Catch::Matchers::VectorContains(
                   javelin::jmap::api::CapabilityError::MissingCalendarsAccountCapability));
}

TEST_CASE("session capability validation fails when primary mail account is missing", "[jmap]")
{
    javelin::jmap::api::Session session{
        .username = "alice@example.com",
        .apiUrl = "https://mail.example.com/jmap/api",
        .downloadUrl =
            "https://mail.example.com/jmap/download/{accountId}/{blobId}/{name}?type={type}",
        .uploadUrl = "https://mail.example.com/jmap/upload/{accountId}",
        .eventSourceUrl = std::nullopt,
        .state = "session-state-1",
        .capabilities =
            {
                .core = true,
                .coreDetails = std::nullopt,
                .mail = true,
                .submission = false,
                .contacts = false,
                .calendars = false,
                .websocket = std::nullopt,
            },
        .accounts =
            {
                {
                    "u1",
                    javelin::jmap::api::Account{
                        .id = "u1",
                        .name = "Personal",
                        .isPersonal = true,
                        .isReadOnly = false,
                        .accountCapabilities = {.mail = true,
                                                .submission = std::nullopt,
                                                .contacts = std::nullopt,
                                                .calendars = std::nullopt},
                    },
                },
            },
        .primaryAccounts = {},
    };

    const auto result = javelin::jmap::api::validateSessionCapabilities(
        session, javelin::jmap::api::RequiredCapabilities{.mail = true});

    REQUIRE_FALSE(result.ok());
    CHECK_THAT(result.errors, Catch::Matchers::VectorContains(
                                  javelin::jmap::api::CapabilityError::MissingPrimaryMailAccount));
}

TEST_CASE("session capability validation fails when submission is required but unsupported",
          "[jmap]")
{
    const auto sessionJson =
        javelin::tests::loadFixture("jmap/session/missing_mail_capability.json");
    const auto result = javelin::jmap::api::parseSession(
        sessionJson, javelin::jmap::api::RequiredCapabilities{.submission = true});

    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error.has_value());
    CHECK(result.error->code ==
          javelin::jmap::api::SessionParseErrorCode::CapabilityValidationFailed);
    REQUIRE(result.error->capabilityErrors.size() == 1);
    CHECK(result.error->capabilityErrors.front() ==
          javelin::jmap::api::CapabilityError::MissingSubmissionCapability);
    CHECK(javelin::jmap::api::toString(result.error->capabilityErrors.front()) ==
          std::string_view("missing_submission_capability"));
}

TEST_CASE("session parser returns a json parse failure for invalid json", "[jmap]")
{
    const auto result = javelin::jmap::api::parseSession("{\"username\":\"alice@example.com\"");

    REQUIRE_FALSE(result.ok());
    REQUIRE(result.error.has_value());
    CHECK(result.error->code == javelin::jmap::api::SessionParseErrorCode::JsonParseFailed);
    CHECK(javelin::jmap::api::toString(result.error->code) ==
          std::string_view("json_parse_failed"));
}
