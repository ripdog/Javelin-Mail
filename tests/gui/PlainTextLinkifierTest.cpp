#include "gui/messageview/PlainTextLinkifier.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("plain text links are clickable and message markup stays escaped", "[gui][message-view]")
{
    const auto html = javelin::gui::messageview::linkifyPlainText(
        QStringLiteral("<b>Visit</b> https://example.test/path?q=one&two."));

    CHECK(html.contains(QStringLiteral("&lt;b&gt;Visit&lt;/b&gt;")));
    CHECK(html.contains(QStringLiteral(
        R"(<a href="https://example.test/path?q=one&amp;two">https://example.test/path?q=one&amp;two</a>.)")));
}

TEST_CASE("plain text email addresses and phone numbers are clickable", "[gui][message-view]")
{
    const auto html = javelin::gui::messageview::linkifyPlainText(
        QStringLiteral("Email person@example.test or call +64 (9) 123-4567."));

    CHECK(html.contains(
        QStringLiteral(R"(<a href="mailto:person@example.test">person@example.test</a>)")));
    CHECK(html.contains(QStringLiteral(R"(<a href="tel:+6491234567">+64 (9) 123-4567</a>)")));
}

TEST_CASE("short numbers in plain text are not treated as phone numbers", "[gui][message-view]")
{
    const auto html =
        javelin::gui::messageview::linkifyPlainText(QStringLiteral("Order 123456 is ready."));

    CHECK_FALSE(html.contains(QStringLiteral("tel:")));
}
