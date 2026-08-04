#include "gui/settings/ConnectionSettingsAdapter.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace javelin::gui::settings;

TEST_CASE("connection settings adapt into application account settings")
{
    const ConnectionSettings settings{
        .id = QStringLiteral("connection"),
        .revision = 42,
        .displayName = QStringLiteral("Personal"),
        .sessionUrl = QStringLiteral("https://mail.example.test/.well-known/jmap"),
        .loginEmail = QStringLiteral("ada@example.test"),
        .apiKey = QStringLiteral("secret"),
        .refreshToken = QStringLiteral("refresh-token"),
        .tokenEndpoint = QStringLiteral("https://auth.example.test/token"),
        .oauthClientId = QStringLiteral("javelin-client"),
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {QStringLiteral("account")},
    };

    const auto actual = toAccountConnectionSettings(settings);

    CHECK(actual.connectionId == "connection");
    CHECK(actual.revision == 42);
    CHECK(actual.sessionUrl == "https://mail.example.test/.well-known/jmap");
    CHECK(actual.loginEmail == "ada@example.test");
    CHECK(actual.apiKey == "secret");
    CHECK(actual.refreshToken == "refresh-token");
    CHECK(actual.tokenEndpoint == "https://auth.example.test/token");
    CHECK(actual.oauthClientId == "javelin-client");
}

TEST_CASE("bootstrap adaptation transfers configured mailbox selections")
{
    const ConnectionSettings settings{
        .id = QStringLiteral("connection"),
        .revision = 3,
        .displayName = {},
        .sessionUrl = QStringLiteral("https://mail.example.test/jmap"),
        .loginEmail = QStringLiteral("ada@example.test"),
        .apiKey = QStringLiteral("secret"),
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {},
    };
    const auto actual =
        toAccountBootstrapIntent(settings, std::vector<std::string>{"inbox", "archive"});

    CHECK(actual.settings.connectionId == "connection");
    CHECK((actual.mailboxIds == std::vector<std::string>{"inbox", "archive"}));
}

TEST_CASE("authenticated connections without cached accounts require initial bootstrap")
{
    ConnectionSettings settings{
        .id = QStringLiteral("connection"),
        .revision = 1,
        .displayName = QStringLiteral("Personal"),
        .sessionUrl = QStringLiteral("https://mail.example.test/jmap"),
        .loginEmail = QStringLiteral("ada@example.test"),
        .apiKey = QStringLiteral("secret"),
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {},
    };

    CHECK(needsInitialAccountBootstrap(settings));

    settings.cachedAccountIds = {QStringLiteral("account")};
    CHECK_FALSE(needsInitialAccountBootstrap(settings));

    settings.cachedAccountIds.clear();
    settings.apiKey.clear();
    CHECK_FALSE(needsInitialAccountBootstrap(settings));
}
