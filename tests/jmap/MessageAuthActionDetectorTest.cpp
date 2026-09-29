#include "jmap/cache/MessageAuthActionDetector.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    using javelin::jmap::cache::MessageAuthActionDetector;
    using javelin::jmap::cache::MessageAuthActionInput;
    using javelin::jmap::cache::MessageAuthActionKind;
} // namespace

TEST_CASE("authentication action detector finds contextual login codes",
          "[cache][message-auth-action]")
{
    MessageAuthActionDetector detector;

    SECTION("six digit code")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Your verification code"),
            .plainText = QStringLiteral("Use 381942 to finish signing in."),
            .html = {},
        });

        REQUIRE(result.action.has_value());
        CHECK(result.action->kind == MessageAuthActionKind::CopyCode);
        CHECK(result.action->value == QStringLiteral("381942"));
        CHECK(result.action->score >= 5);
    }

    SECTION("prefixed alphanumeric code")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Security code"),
            .plainText = QStringLiteral("Your authentication code is G-3d523."),
            .html = {},
        });

        REQUIRE(result.action.has_value());
        CHECK(result.action->kind == MessageAuthActionKind::CopyCode);
        CHECK(result.action->value == QStringLiteral("G-3d523"));
    }

    SECTION("unicode digits are normalized")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Login code"),
            .plainText = QString::fromUtf8("Code: １２３４５６"),
            .html = {},
        });

        REQUIRE(result.action.has_value());
        CHECK(result.action->kind == MessageAuthActionKind::CopyCode);
        CHECK(result.action->value == QStringLiteral("123456"));
    }

    SECTION("PIN wording is strong authentication context")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Account access"),
            .plainText = QStringLiteral("Your PIN is 4812."),
            .html = {},
        });

        REQUIRE(result.action.has_value());
        CHECK(result.action->kind == MessageAuthActionKind::CopyCode);
        CHECK(result.action->value == QStringLiteral("4812"));
    }

    SECTION("HTML text supplements a sparse plain-text alternative")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Account access"),
            .plainText = QStringLiteral("Open this message to continue."),
            .html = QStringLiteral("<p>Your verification code is <strong>381942</strong>.</p>"),
        });

        REQUIRE(result.action.has_value());
        CHECK(result.action->kind == MessageAuthActionKind::CopyCode);
        CHECK(result.action->value == QStringLiteral("381942"));
        CHECK(result.debugSummary().contains(QStringLiteral("body-source=plain-text+html")));
    }
}

TEST_CASE("authentication action detector rejects common numeric false positives",
          "[cache][message-auth-action]")
{
    MessageAuthActionDetector detector;

    SECTION("order reference")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Your order shipped"),
            .plainText = QStringLiteral("Order reference 884211 is now with the courier."),
            .html = {},
        });

        CHECK_FALSE(result.action.has_value());
        CHECK(result.debugSummary().contains(QStringLiteral("order/reference/amount context")));
    }

    SECTION("year in authentication-looking prose")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Account security update"),
            .plainText = QStringLiteral("Verification codes introduced in 2026 remain supported."),
            .html = {},
        });

        CHECK_FALSE(result.action.has_value());
        CHECK(result.debugSummary().contains(QStringLiteral("looks like a year")));
    }

    SECTION("phone number")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Contact us"),
            .plainText = QStringLiteral("Call our support team on +64 21 555 1234."),
            .html = {},
        });

        CHECK_FALSE(result.action.has_value());
        CHECK(result.debugSummary().contains(QStringLiteral("phone number")));
    }

    SECTION("shopping does not count as PIN authentication context")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Updated Comixology app and shopping experience are here!"),
            .plainText = QStringLiteral("Campaign reference 009595"),
            .html = {},
        });

        CHECK_FALSE(result.action.has_value());
    }

    SECTION("time suffix does not become an alphanumeric code")
    {
        const auto result = detector.detect({
            .subject =
                QStringLiteral("Update #62 from Code 8 - a film from Robbie & Stephen Amell"),
            .plainText = QStringLiteral("posted by Robbie Amell Aug 20, 2025 • 6:25AM PDT"),
            .html = {},
        });

        CHECK_FALSE(result.action.has_value());
        CHECK(result.debugSummary().contains(QStringLiteral("part of a time")));
    }

    SECTION("Tapping does not count as PIN authentication context")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral(
                "Harder, Better, Faster, Stronger, Prettier - These Weeks in Firefox"),
            .plainText =
                QStringLiteral("Bug 1680865 - Tapping the Search or enter address touch bar item."),
            .html = {},
        });

        CHECK_FALSE(result.action.has_value());
    }
}

TEST_CASE("authentication action detector finds high-confidence HTML action links",
          "[cache][message-auth-action]")
{
    MessageAuthActionDetector detector;

    const auto result = detector.detect({
        .subject = QStringLiteral("Sign in to Example"),
        .plainText = QStringLiteral("Use the button below to continue."),
        .html = QStringLiteral(
            "<p>Use this link to access your account.</p>"
            "<a href=\"https://accounts.example.test/auth/magic?token=secret-value\">Sign in</a>"),
    });

    REQUIRE(result.action.has_value());
    CHECK(result.action->kind == MessageAuthActionKind::OpenLink);
    CHECK(result.action->value ==
          QStringLiteral("https://accounts.example.test/auth/magic?token=secret-value"));
    CHECK(result.debugSummary().contains(QStringLiteral("host=accounts.example.test")));
    CHECK_FALSE(result.debugSummary().contains(QStringLiteral("secret-value")));
}

TEST_CASE("authentication action detector requires corroboration for generic verify links",
          "[cache][message-auth-action]")
{
    MessageAuthActionDetector detector;

    SECTION("developer verify wording alone is insufficient")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("These Weeks in Firefox"),
            .plainText = QStringLiteral("Recent development work"),
            .html =
                QStringLiteral("<a href=\"https://bugzilla.mozilla.org/show_bug.cgi?id=1824870\">"
                               "Fixed a test-verify permafailure caused by an incomplete teardown"
                               "</a>"),
        });

        CHECK_FALSE(result.action.has_value());
    }

    SECTION("verify link with authentication context still passes")
    {
        const auto result = detector.detect({
            .subject = QStringLiteral("Verify your email"),
            .plainText = QStringLiteral("Please verify your email address to finish signing in."),
            .html = QStringLiteral(
                "<a href=\"https://accounts.example.test/verify?token=secret\">Verify</a>"),
        });

        REQUIRE(result.action.has_value());
        CHECK(result.action->kind == MessageAuthActionKind::OpenLink);
    }
}

TEST_CASE("authentication action detector rejects footer links", "[cache][message-auth-action]")
{
    MessageAuthActionDetector detector;

    const auto result = detector.detect({
        .subject = QStringLiteral("Monthly account update"),
        .plainText = QStringLiteral("Your monthly account update is ready."),
        .html = QStringLiteral(
            "<p>Your monthly update.</p>"
            "<a href=\"https://example.test/preferences?token=secret-value\">Unsubscribe</a>"),
    });

    CHECK_FALSE(result.action.has_value());
    CHECK(result.debugSummary().contains(QStringLiteral("negative link text")));
    CHECK_FALSE(result.debugSummary().contains(QStringLiteral("secret-value")));
}
