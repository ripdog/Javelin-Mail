#include "gui/settings/GuiSettings.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("GUI settings reject stale revisions before applying an update",
          "[gui][settings][revision]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.revision = {.value = 7};
    snapshot.undoSendDelaySeconds = 10;
    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};

    javelin::protocol::SettingsUpdate first;
    first.undoSendDelaySeconds = 20;
    REQUIRE_FALSE(settings.update({.value = 7}, std::move(first)).has_value());
    CHECK(settings.snapshot().revision.value == 8);
    CHECK(settings.undoSendDelaySeconds() == 20);

    javelin::protocol::SettingsUpdate stale;
    stale.undoSendDelaySeconds = 30;
    const auto error = settings.update({.value = 7}, std::move(stale));
    REQUIRE(error.has_value());
    CHECK(error->code == javelin::protocol::BoundaryErrorCode::StaleSettingsRevision);
    CHECK(settings.snapshot().revision.value == 8);
    CHECK(settings.undoSendDelaySeconds() == 20);
}

TEST_CASE("GUI settings expose daemon snapshots without reading QSettings", "[gui][settings]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.accounts.push_back({
        .id = QStringLiteral("connection-1"),
        .revision = 4,
        .displayName = QStringLiteral("Personal"),
        .sessionUrl = QStringLiteral("https://mail.example.test/.well-known/jmap"),
        .loginEmail = QStringLiteral("alice@example.test"),
        .apiKey = QStringLiteral("secret"),
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {QStringLiteral("account-1")},
    });
    snapshot.notificationMailboxSelections.push_back({
        .accountId = QStringLiteral("account-1"),
        .mailboxIds = {QStringLiteral("inbox")},
        .configured = true,
    });
    snapshot.attachments = {.alwaysAsk = false, .directory = QStringLiteral("/tmp/mail")};

    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};
    const auto account = settings.accountForCachedId(QStringLiteral("account-1"));
    CHECK(account.id == QStringLiteral("connection-1"));
    CHECK(account.displayName == QStringLiteral("Personal"));
    CHECK(settings.notificationMailboxIds(QStringLiteral("account-1")) ==
          QStringList{QStringLiteral("inbox")});
    CHECK(settings.hasNotificationMailboxSelection(QStringLiteral("account-1")));
    CHECK_FALSE(settings.attachmentSaveSettings().alwaysAsk);
    CHECK(settings.attachmentSaveSettings().directory == QStringLiteral("/tmp/mail"));
}

TEST_CASE("GUI settings ignore identical workspace updates", "[gui][settings][workspace]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.revision = {.value = 4};
    snapshot.workspace.mainWindowState = QByteArrayLiteral("window-state");

    const auto workspace = snapshot.workspace;
    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};

    CHECK_FALSE(settings.updateWorkspace(workspace).has_value());
    CHECK(settings.snapshot().revision.value == 4);
    CHECK(settings.workspaceSettings() == workspace);
}
