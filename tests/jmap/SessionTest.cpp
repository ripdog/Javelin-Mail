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
                        .accountCapabilities = {.mail = true, .submission = false},
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
