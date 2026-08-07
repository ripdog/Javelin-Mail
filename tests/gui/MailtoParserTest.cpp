#include "gui/compose/MailtoParser.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

TEST_CASE("mailto parser decodes compose fields", "[gui][compose][mailto]")
{
    const auto parsed = javelin::gui::compose::parseMailtoUri(
        QStringLiteral("mailto:alice@example.test,bob@example.test?cc=carol%40example.test"
                       "&bcc=dave%40example.test&subject=Hello%20there"
                       "&body=Line%201%0ALine%202"));

    REQUIRE(parsed.has_value());
    REQUIRE(parsed->to.size() == 2);
    CHECK(parsed->to[0].email == "alice@example.test");
    CHECK(parsed->to[1].email == "bob@example.test");
    REQUIRE(parsed->cc.size() == 1);
    CHECK(parsed->cc.front().email == "carol@example.test");
    REQUIRE(parsed->bcc.size() == 1);
    CHECK(parsed->bcc.front().email == "dave@example.test");
    CHECK(parsed->subject == std::optional<std::string>{"Hello there"});
    CHECK(parsed->body == std::optional<std::string>{"Line 1\nLine 2"});
}

TEST_CASE("mailto parser accepts query recipients and rejects other schemes",
          "[gui][compose][mailto]")
{
    const auto parsed = javelin::gui::compose::parseMailtoUri(
        QStringLiteral("MAILTO:?to=alice%40example.test&to=bob%40example.test"));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->to.size() == 2);
    CHECK(parsed->to[0].email == "alice@example.test");
    CHECK(parsed->to[1].email == "bob@example.test");

    CHECK_FALSE(
        javelin::gui::compose::parseMailtoUri(QStringLiteral("https://example.test/")).has_value());
}
