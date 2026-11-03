#include "gui/settings/GuiSettings.h"
#include "gui/compose/ComposeUiPreferences.h"

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

TEST_CASE("GUI settings expose account and mailbox values from the daemon snapshot",
          "[gui][settings]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.accounts.push_back({
        .id = QStringLiteral("connection-1"),
        .revision = 4,
        .displayName = QStringLiteral("Personal"),
        .sessionUrl = QStringLiteral("https://mail.example.test/.well-known/jmap"),
        .loginEmail = QStringLiteral("alice@example.test"),
        .tokenEndpoint = {},
        .oauthClientId = {},
        .hasCredentials = true,
        .credentialHandle = {},
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {QStringLiteral("account-1")},
    });
    snapshot.notificationMailboxSelections.push_back({
        .accountId = QStringLiteral("account-1"),
        .mailboxIds = {QStringLiteral("inbox")},
    });
    snapshot.attachments = {.alwaysAsk = false, .directory = QStringLiteral("/tmp/mail")};

    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};
    const auto account = settings.accountForCachedId(QStringLiteral("account-1"));
    CHECK(account.id == QStringLiteral("connection-1"));
    CHECK(account.displayName == QStringLiteral("Personal"));
    CHECK(settings.notificationMailboxIds(QStringLiteral("account-1")) ==
          QStringList{QStringLiteral("inbox")});
    CHECK_FALSE(settings.attachmentSaveSettings().alwaysAsk);
    CHECK(settings.attachmentSaveSettings().directory == QStringLiteral("/tmp/mail"));
}

TEST_CASE("GUI settings keep absent mailbox selections empty", "[gui][settings][mailbox]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.accounts.push_back({
        .id = QStringLiteral("connection-1"),
        .revision = 0,
        .displayName = {},
        .sessionUrl = {},
        .loginEmail = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
        .hasCredentials = false,
        .credentialHandle = {},
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {QStringLiteral("account-1")},
    });

    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};
    CHECK(settings.syncedMailboxIds(QStringLiteral("account-1")).isEmpty());
    CHECK(settings.notificationMailboxIds(QStringLiteral("account-1")).isEmpty());
}

TEST_CASE("GUI settings add an Inbox notification selection without changing sync settings",
          "[gui][settings][mailbox][notifications]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.revision = {.value = 11};
    snapshot.syncedMailboxSelections = {
        {.accountId = QStringLiteral("account-1"), .mailboxIds = {QStringLiteral("archive")}},
    };
    snapshot.notificationMailboxSelections = {
        {.accountId = QStringLiteral("account-2"), .mailboxIds = {QStringLiteral("other-inbox")}},
    };
    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};

    REQUIRE_FALSE(
        settings
            .ensureNotificationMailboxSelected(QStringLiteral("account-1"), QStringLiteral("inbox"))
            .has_value());
    CHECK(settings.notificationMailboxIds(QStringLiteral("account-1")) ==
          QStringList{QStringLiteral("inbox")});
    CHECK(settings.syncedMailboxIds(QStringLiteral("account-1")) ==
          QStringList{QStringLiteral("archive")});
    CHECK(settings.notificationMailboxIds(QStringLiteral("account-2")) ==
          QStringList{QStringLiteral("other-inbox")});
    CHECK(settings.snapshot().revision.value == 12);

    REQUIRE_FALSE(
        settings
            .ensureNotificationMailboxSelected(QStringLiteral("account-1"), QStringLiteral("inbox"))
            .has_value());
    CHECK(settings.snapshot().revision.value == 12);
}

TEST_CASE("compose format preference persists through workspace settings",
          "[gui][settings][compose]")
{
    javelin::protocol::SettingsSnapshot snapshot;
    snapshot.revision = {.value = 9};
    snapshot.workspace.composeRichTextDefault = false;
    javelin::gui::settings::GuiSettings settings{std::move(snapshot)};

    CHECK_FALSE(javelin::gui::compose::ComposeUiPreferences::richTextDefault(settings));
    REQUIRE_FALSE(javelin::gui::compose::ComposeUiPreferences::setRichTextDefault(settings, true)
                      .has_value());
    CHECK(javelin::gui::compose::ComposeUiPreferences::richTextDefault(settings));
    CHECK(settings.snapshot().revision.value == 10);

    REQUIRE_FALSE(javelin::gui::compose::ComposeUiPreferences::setRichTextDefault(settings, true)
                      .has_value());
    CHECK(settings.snapshot().revision.value == 10);
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

namespace
{
    javelin::protocol::SettingsSnapshot preferencesSnapshot()
    {
        javelin::protocol::SettingsSnapshot snapshot;
        snapshot.revision = {.value = 3};
        javelin::protocol::AccountSettings account;
        account.id = QStringLiteral("connection-1");
        account.revision = 1;
        snapshot.accounts.push_back(std::move(account));
        snapshot.undoSendDelaySeconds = 10;
        return snapshot;
    }
} // namespace

TEST_CASE("a stale preferences update is rebased when only the workspace changed",
          "[gui][settings][revision]")
{
    const auto baseline = preferencesSnapshot();
    javelin::gui::settings::GuiSettings settings{baseline};

    // The main window saves its geometry while Preferences is open.
    auto workspace = settings.workspaceSettings();
    workspace.mainWindowState = QByteArrayLiteral("window-state");
    REQUIRE_FALSE(settings.updateWorkspace(std::move(workspace)).has_value());
    REQUIRE(settings.snapshot().revision.value == 4);

    javelin::protocol::SettingsUpdate update;
    update.accounts = std::vector<javelin::protocol::AccountSettings>{};
    update.undoSendDelaySeconds = 25;
    REQUIRE(settings.update(baseline.revision, update).has_value());

    REQUIRE_FALSE(settings.updateRebased(baseline, std::move(update)).has_value());
    CHECK(settings.snapshot().accounts.empty());
    CHECK(settings.undoSendDelaySeconds() == 25);
    CHECK(settings.snapshot().workspace.mainWindowState == QByteArrayLiteral("window-state"));
    CHECK(settings.snapshot().revision.value == 5);
}

TEST_CASE("a stale preferences update is refused when a section it replaces changed",
          "[gui][settings][revision]")
{
    const auto baseline = preferencesSnapshot();
    javelin::gui::settings::GuiSettings settings{baseline};

    // The daemon marks the account as needing sign-in while Preferences is open.
    auto accounts = settings.snapshot().accounts;
    accounts.front().reauthenticationRequired = true;
    javelin::protocol::SettingsUpdate daemonUpdate;
    daemonUpdate.accounts = std::move(accounts);
    REQUIRE_FALSE(settings.update(std::move(daemonUpdate)).has_value());

    javelin::protocol::SettingsUpdate update;
    update.accounts = std::vector<javelin::protocol::AccountSettings>{};
    const auto error = settings.updateRebased(baseline, std::move(update));

    REQUIRE(error.has_value());
    CHECK(error->code == javelin::protocol::BoundaryErrorCode::StaleSettingsRevision);
    REQUIRE(settings.snapshot().accounts.size() == 1);
    CHECK(settings.snapshot().accounts.front().reauthenticationRequired);
    CHECK(settings.snapshot().revision.value == 4);
}

TEST_CASE("a rebased preferences update ignores changes to sections it leaves alone",
          "[gui][settings][revision]")
{
    const auto baseline = preferencesSnapshot();
    javelin::gui::settings::GuiSettings settings{baseline};

    javelin::protocol::SettingsUpdate other;
    other.undoSendDelaySeconds = 30;
    REQUIRE_FALSE(settings.update(std::move(other)).has_value());

    javelin::protocol::SettingsUpdate update;
    update.accounts = std::vector<javelin::protocol::AccountSettings>{};
    REQUIRE_FALSE(settings.updateRebased(baseline, std::move(update)).has_value());
    CHECK(settings.snapshot().accounts.empty());
    CHECK(settings.undoSendDelaySeconds() == 30);
}
