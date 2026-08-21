#include "gui/compose/EmailAddressText.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("email address formatting quotes and escapes structured display names")
{
    using javelin::gui::compose::displayAddress;

    CHECK(displayAddress({.name = std::string{"Doe, Jane"}, .email = "jane@example.test"}) ==
          QStringLiteral("\"Doe, Jane\" <jane@example.test>"));
    CHECK(
        displayAddress({.name = std::string{"Jane \"QA\" \\ Ops"}, .email = "jane@example.test"}) ==
        QStringLiteral("\"Jane \\\"QA\\\" \\\\ Ops\" <jane@example.test>"));
    CHECK(displayAddress({.name = std::string{"Jane Doe"}, .email = "jane@example.test"}) ==
          QStringLiteral("Jane Doe <jane@example.test>"));
}

TEST_CASE("email address parsing respects quoted strings and angle brackets")
{
    using javelin::gui::compose::parseAddressList;

    const auto parsed = parseAddressList(
        QStringLiteral("\"Doe, Jane\" <jane@example.test>, \"Smith, John\" <john@example.test>; "
                       "plain@example.test"));

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 3);
    CHECK(parsed->at(0).name == std::optional<std::string>{"Doe, Jane"});
    CHECK(parsed->at(0).email == "jane@example.test");
    CHECK(parsed->at(1).name == std::optional<std::string>{"Smith, John"});
    CHECK(parsed->at(1).email == "john@example.test");
    CHECK_FALSE(parsed->at(2).name.has_value());
    CHECK(parsed->at(2).email == "plain@example.test");
}

TEST_CASE("email address formatting and parsing round trip escaped display names")
{
    using javelin::gui::compose::formatAddresses;
    using javelin::gui::compose::parseAddressList;

    const std::vector<javelin::jmap::domain::EmailAddress> addresses{
        {.name = std::string{"Doe, Jane"}, .email = "jane@example.test"},
        {.name = std::string{"Jane \"QA\" \\ Ops"}, .email = "qa@example.test"},
    };

    const auto parsed = parseAddressList(formatAddresses(addresses));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == addresses.size());
    CHECK(parsed->at(0).name == addresses[0].name);
    CHECK(parsed->at(0).email == addresses[0].email);
    CHECK(parsed->at(1).name == addresses[1].name);
    CHECK(parsed->at(1).email == addresses[1].email);
}

TEST_CASE("lenient email address parsing skips invalid recipient tokens")
{
    const auto parsed = javelin::gui::compose::parseAddressList(
        QStringLiteral("valid@example.test, not-an-address, other@example.test"), false);

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
    CHECK(parsed->at(0).email == "valid@example.test");
    CHECK(parsed->at(1).email == "other@example.test");
}
