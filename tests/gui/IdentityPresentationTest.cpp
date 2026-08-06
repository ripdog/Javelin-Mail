#include "gui/compose/IdentityPresentation.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    [[nodiscard]] javelin::jmap::domain::Identity
    identity(std::string name, std::string email,
             std::optional<std::string> textSignature = std::nullopt,
             std::optional<std::string> htmlSignature = std::nullopt)
    {
        return {
            .id = "identity-1",
            .name = std::move(name),
            .email = std::move(email),
            .replyTo = {},
            .bcc = {},
            .textSignature = std::move(textSignature),
            .htmlSignature = std::move(htmlSignature),
            .mayDelete = true,
        };
    }
} // namespace

TEST_CASE("identity signature preview uses the first non-empty plain-text line", "[gui][identity]")
{
    const auto value = identity("Alice", "alice@example.test", "\n  Regards, Alice  \nExample Ltd");
    CHECK(javelin::gui::compose::identitySignaturePreview(value) ==
          QStringLiteral("Regards, Alice"));
}

TEST_CASE("identity signature preview falls back to HTML text", "[gui][identity]")
{
    const auto value = identity("Alice", "alice@example.test", std::nullopt,
                                "<p><br></p><p><b>Example Ltd</b></p>");
    CHECK(javelin::gui::compose::identitySignaturePreview(value) == QStringLiteral("Example Ltd"));
}

TEST_CASE("compose identity labels distinguish same-address variants", "[gui][identity]")
{
    const auto signedValue = identity("Alice", "alice@example.test", "Regards, Alice");
    const auto unsignedValue = identity("Alice", "alice@example.test", "", "");

    CHECK(javelin::gui::compose::composeIdentityDisplayText(signedValue, QStringLiteral("Personal"),
                                                            2, false) ==
          QStringLiteral("Alice <alice@example.test> — Regards, Alice"));
    CHECK(javelin::gui::compose::composeIdentityDisplayText(unsignedValue,
                                                            QStringLiteral("Personal"), 2, true) ==
          QStringLiteral("Alice <alice@example.test> — No signature — Personal"));
}

TEST_CASE("single identities omit signature previews", "[gui][identity]")
{
    const auto value = identity("Alice", "alice@example.test", "Regards, Alice");
    CHECK(javelin::gui::compose::composeIdentityDisplayText(value, QStringLiteral("Personal"), 1,
                                                            false) ==
          QStringLiteral("Alice <alice@example.test>"));
}
