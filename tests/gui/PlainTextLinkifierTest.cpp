#include "gui/messageview/PlainTextLinkifier.h"
#include "gui/messageview/ExternalMessageLinkPolicy.h"

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

TEST_CASE("message links distinguish browser, compose, and rejected schemes", "[gui][message-view]")
{
    using javelin::gui::messageview::MessageLinkAction;
    using javelin::gui::messageview::messageLinkAction;

    CHECK(messageLinkAction(QUrl{QStringLiteral("https://example.test/unsubscribe")}) ==
          MessageLinkAction::OpenExternal);
    CHECK(messageLinkAction(QUrl{QStringLiteral("HTTP://example.test/")}) ==
          MessageLinkAction::OpenExternal);
    CHECK(messageLinkAction(QUrl{QStringLiteral("mailto:list@example.test")}) ==
          MessageLinkAction::ComposeMail);
    CHECK(messageLinkAction(QUrl{QStringLiteral("javascript:alert(1)")}) ==
          MessageLinkAction::Reject);
    CHECK(messageLinkAction(QUrl{QStringLiteral("data:text/plain,hello")}) ==
          MessageLinkAction::Reject);
    CHECK(messageLinkAction(QUrl{QStringLiteral("file:///tmp/message")}) ==
          MessageLinkAction::Reject);
}
