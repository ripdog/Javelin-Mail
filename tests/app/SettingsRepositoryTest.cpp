#include "app/SettingsRepository.h"
#include "gui/shell/MainWindowStateStore.h"

#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariantMap>

#include <memory>
#include <utility>
#include <variant>

namespace
{

    using javelin::app::AccountCredentialSecrets;
    using javelin::app::MemoryAccountCredentialStore;
    using javelin::app::SettingsReadResult;
    using javelin::app::SettingsRepository;
    using javelin::app::SettingsRepositoryError;
    using javelin::protocol::AccountSettings;
    using javelin::protocol::SettingsSnapshot;
    using javelin::protocol::SettingsUpdate;

    [[nodiscard]] MemoryAccountCredentialStore& credentialStore()
    {
        static MemoryAccountCredentialStore store;
        return store;
    }

    [[nodiscard]] SettingsRepository repositoryFor(const QString& path)
    {
        return SettingsRepository{std::make_unique<QSettings>(path, QSettings::IniFormat),
                                  &credentialStore()};
    }

    [[nodiscard]] SettingsUpdate emptyUpdate()
    {
        return {.accounts = std::nullopt,
                .syncedMailboxSelections = std::nullopt,
                .notificationMailboxSelections = std::nullopt,
                .remoteContentSenders = std::nullopt,
                .remoteContentDomains = std::nullopt,
                .appearance = std::nullopt,
                .attachments = std::nullopt,
                .undoSendDelaySeconds = std::nullopt,
                .undoSendUsesDialog = std::nullopt,
                .workspace = std::nullopt};
    }

    void writeLegacySettings(const QString& path)
    {
        QSettings settings{path, QSettings::IniFormat};
        settings.beginGroup(QStringLiteral("accounts"));
        settings.beginWriteArray(QStringLiteral("size"), 1);
        settings.setArrayIndex(0);
        settings.setValue(QStringLiteral("id"), QStringLiteral("configured-1"));
        settings.setValue(QStringLiteral("revision"), static_cast<qulonglong>(4));
        settings.setValue(QStringLiteral("displayName"), QStringLiteral("Personal"));
        settings.setValue(QStringLiteral("sessionUrl"),
                          QStringLiteral("https://example.test/jmap"));
        settings.setValue(QStringLiteral("loginEmail"), QStringLiteral("user@example.test"));
        settings.setValue(QStringLiteral("apiKey"), QStringLiteral("secret"));
        settings.setValue(QStringLiteral("cachedAccountIds"),
                          QStringList{QStringLiteral("account-1")});
        settings.endArray();
        settings.endGroup();
        settings.setValue(QStringLiteral("mailboxSync/account-1/mailboxIds"),
                          QStringList{QStringLiteral("inbox"), QStringLiteral("archive")});
        settings.setValue(QStringLiteral("mailboxNotifications/account-1/mailboxIds"),
                          QStringList{QStringLiteral("inbox")});
        settings.setValue(QStringLiteral("remoteContent/allowedSenders"),
                          QStringList{QStringLiteral("sender@example.test")});
        settings.setValue(QStringLiteral("remoteContent/allowedDomains"),
                          QStringList{QStringLiteral("example.test")});
        settings.setValue(QStringLiteral("translation/enabled"), false);
        settings.setValue(QStringLiteral("translation/apiKeyOverride"),
                          QStringLiteral(" translation-key "));
        settings.setValue(QStringLiteral("translation/targetLanguage"), QStringLiteral("JA"));
        settings.setValue(QStringLiteral("translation/autoTranslateSenders"),
                          QStringList{QStringLiteral(" Sender@Example.test "),
                                      QStringLiteral("sender@example.test")});
        settings.setValue(QStringLiteral("translation/autoTranslateDomains"),
                          QStringList{QStringLiteral(" Example.test ")});
        settings.setValue(QStringLiteral("messageAppearance/colorMode"), 2);
        settings.setValue(QStringLiteral("attachments/alwaysAsk"), false);
        settings.setValue(QStringLiteral("attachments/directory"), QStringLiteral("/tmp/mail"));
        settings.setValue(QStringLiteral("compose/undoSendDelaySeconds"), 30);
        settings.setValue(QStringLiteral("compose/undoSendUsesDialog"), true);
        settings.beginGroup(QStringLiteral("mainWindow"));
        settings.setValue(QStringLiteral("geometry"), QByteArrayLiteral("legacy-geometry"));
        settings.setValue(QStringLiteral("activeTabIndex"), 0);
        settings.beginWriteArray(QStringLiteral("tabs"), 1);
        settings.setArrayIndex(0);
        settings.setValue(QStringLiteral("type"), QStringLiteral("mailbox"));
        settings.setValue(QStringLiteral("accountId"), QStringLiteral("account-1"));
        settings.setValue(QStringLiteral("title"), QStringLiteral("Inbox"));
        settings.setValue(QStringLiteral("mailboxId"), QStringLiteral("inbox"));
        settings.endArray();
        settings.endGroup();
        settings.setValue(QStringLiteral("calendar/colorOverrides"),
                          QVariantMap{{QStringLiteral("calendar-1"), QColor{0x12, 0x34, 0x56}}});
        settings.sync();
        REQUIRE(settings.status() == QSettings::NoError);
    }

} // namespace

TEST_CASE("settings repository creates and persists its schema identity", "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("settings.ini"));

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* snapshot = std::get_if<SettingsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->revision.value == 0);
    CHECK(snapshot->schemaVersion == 7);
    CHECK(snapshot->undoSendDelaySeconds == 10);
    CHECK_FALSE(snapshot->undoSendUsesDialog);

    QSettings persisted{path, QSettings::IniFormat};
    CHECK(persisted.value(QStringLiteral("settings/schemaVersion")).toUInt() == 7);
    CHECK(persisted.value(QStringLiteral("settings/revision")).toULongLong() == 0);
}

TEST_CASE("settings repository migrates the complete legacy operational shape", "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("legacy.ini"));
    writeLegacySettings(path);
    REQUIRE_FALSE(credentialStore().remove(QStringLiteral("configured-1")).has_value());

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* snapshot = std::get_if<SettingsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    REQUIRE(snapshot->accounts.size() == 1);
    CHECK(snapshot->accounts.front() ==
          AccountSettings{.id = QStringLiteral("configured-1"),
                          .revision = 4,
                          .displayName = QStringLiteral("Personal"),
                          .sessionUrl = QStringLiteral("https://example.test/jmap"),
                          .loginEmail = QStringLiteral("user@example.test"),
                          .tokenEndpoint = {},
                          .oauthClientId = {},
                          .hasCredentials = false,
                          .credentialHandle = {},
                          .tokenExpiresAtEpochSeconds = 0,
                          .cachedAccountIds = {QStringLiteral("account-1")}});
    REQUIRE(snapshot->syncedMailboxSelections.size() == 1);
    CHECK(snapshot->syncedMailboxSelections.front().mailboxIds ==
          std::vector<QString>{QStringLiteral("inbox"), QStringLiteral("archive")});
    REQUIRE(snapshot->notificationMailboxSelections.size() == 1);
    CHECK(snapshot->appearance.messageColorMode == 2);
    CHECK_FALSE(snapshot->attachments.alwaysAsk);
    CHECK(snapshot->attachments.directory == QStringLiteral("/tmp/mail"));
    CHECK(snapshot->undoSendDelaySeconds == 30);
    CHECK(snapshot->undoSendUsesDialog);
    const auto workspace =
        javelin::gui::shell::deserializeMainWindowState(snapshot->workspace.mainWindowState, {});
    CHECK(workspace.geometry == QByteArrayLiteral("legacy-geometry"));
    REQUIRE(workspace.tabs.size() == 1);
    CHECK(std::get<javelin::gui::shell::PersistedMailboxTab>(workspace.tabs.front()).mailboxId ==
          "inbox");
    REQUIRE(snapshot->workspace.calendarColorOverrides.size() == 1);
    CHECK(snapshot->workspace.calendarColorOverrides.front().calendarId ==
          QStringLiteral("calendar-1"));
    CHECK(snapshot->workspace.calendarColorOverrides.front().color == QStringLiteral("#123456"));

    QSettings migrated{path, QSettings::IniFormat};
    CHECK(migrated.value(QStringLiteral("settings/schemaVersion")).toUInt() == 7);
    CHECK(migrated.value(QStringLiteral("settings/revision")).toULongLong() == 0);
    CHECK_FALSE(migrated.value(QStringLiteral("translation/enabled")).toBool());
    CHECK(migrated.value(QStringLiteral("translation/targetLanguage")).toString() ==
          QStringLiteral("JA"));
    CHECK_FALSE(migrated.contains(QStringLiteral("mainWindow/geometry")));
    CHECK_FALSE(migrated.contains(QStringLiteral("calendar/colorOverrides")));
    migrated.beginGroup(QStringLiteral("accounts"));
    REQUIRE(migrated.beginReadArray(QStringLiteral("size")) == 1);
    migrated.setArrayIndex(0);
    CHECK_FALSE(migrated.contains(QStringLiteral("apiKey")));
    CHECK_FALSE(migrated.contains(QStringLiteral("refreshToken")));
    CHECK_FALSE(migrated.contains(QStringLiteral("registrationAccessToken")));
    migrated.endArray();
    migrated.endGroup();

    const auto stored = credentialStore().load(QStringLiteral("configured-1"));
    const auto* credentials = std::get_if<std::optional<AccountCredentialSecrets>>(&stored);
    REQUIRE(credentials != nullptr);
    REQUIRE(credentials->has_value());
    CHECK((*credentials)->accessToken == QStringLiteral("secret"));
}

TEST_CASE("settings repository migrates schema one workspace state", "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("schema-one.ini"));
    QSettings settings{path, QSettings::IniFormat};
    settings.setValue(QStringLiteral("settings/schemaVersion"), 1);
    settings.setValue(QStringLiteral("settings/revision"), 9);
    settings.setValue(QStringLiteral("mainWindow/activeTabIndex"), 4);
    settings.sync();
    REQUIRE(settings.status() == QSettings::NoError);

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* snapshot = std::get_if<SettingsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->schemaVersion == 7);
    CHECK(snapshot->revision.value == 0);
    CHECK(javelin::gui::shell::deserializeMainWindowState(snapshot->workspace.mainWindowState, {})
              .activeTabIndex == 4);

    QSettings migrated{path, QSettings::IniFormat};
    CHECK(migrated.value(QStringLiteral("settings/schemaVersion")).toUInt() == 7);
    CHECK_FALSE(migrated.contains(QStringLiteral("mainWindow/activeTabIndex")));
}

TEST_CASE("schema four migration retries legacy OAuth grants blocked by missing resources",
          "[app][settings][oauth]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("schema-four-oauth.ini"));
    QSettings settings{path, QSettings::IniFormat};
    settings.setValue(QStringLiteral("settings/schemaVersion"), 4);
    settings.beginGroup(QStringLiteral("accounts"));
    settings.beginWriteArray(QStringLiteral("size"), 1);
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("id"), QStringLiteral("connection-1"));
    settings.setValue(QStringLiteral("sessionUrl"),
                      QStringLiteral("https://mail.example.test/.well-known/jmap"));
    settings.setValue(QStringLiteral("loginEmail"), QStringLiteral("user@example.test"));
    settings.setValue(QStringLiteral("tokenEndpoint"),
                      QStringLiteral("https://auth.example.test/token"));
    settings.setValue(QStringLiteral("oauthClientId"), QStringLiteral("client-id"));
    settings.setValue(QStringLiteral("oauthResource"), QString{});
    settings.setValue(QStringLiteral("reauthenticationRequired"), true);
    settings.endArray();
    settings.endGroup();
    settings.sync();
    REQUIRE(settings.status() == QSettings::NoError);

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* snapshot = std::get_if<SettingsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->schemaVersion == 7);
    REQUIRE(snapshot->accounts.size() == 1);
    CHECK_FALSE(snapshot->accounts.front().reauthenticationRequired);

    QSettings migrated{path, QSettings::IniFormat};
    CHECK(migrated.value(QStringLiteral("settings/schemaVersion")).toUInt() == 7);
}

TEST_CASE("schema five migration adds an empty email context menu override", "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("schema-five.ini"));
    QSettings settings{path, QSettings::IniFormat};
    settings.setValue(QStringLiteral("settings/schemaVersion"), 5);
    settings.setValue(QStringLiteral("workspace/formatVersion"), 1);
    settings.setValue(QStringLiteral("workspace/mainWindowState"),
                      QByteArrayLiteral("schema-five-window"));
    settings.sync();
    REQUIRE(settings.status() == QSettings::NoError);

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* snapshot = std::get_if<SettingsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->schemaVersion == 7);
    CHECK(snapshot->workspace.mainWindowState == QByteArrayLiteral("schema-five-window"));
    CHECK(snapshot->workspace.emailContextMenuLayout.empty());

    QSettings migrated{path, QSettings::IniFormat};
    CHECK(migrated.value(QStringLiteral("settings/schemaVersion")).toUInt() == 7);
}

TEST_CASE("schema six migration adds an empty calendar event context menu override",
          "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("schema-six.ini"));
    QSettings settings{path, QSettings::IniFormat};
    settings.setValue(QStringLiteral("settings/schemaVersion"), 6);
    settings.setValue(QStringLiteral("workspace/formatVersion"), 1);
    settings.setValue(QStringLiteral("workspace/mainWindowState"),
                      QByteArrayLiteral("schema-six-window"));
    settings.sync();
    REQUIRE(settings.status() == QSettings::NoError);

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* snapshot = std::get_if<SettingsSnapshot>(&result);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->schemaVersion == 7);
    CHECK(snapshot->workspace.mainWindowState == QByteArrayLiteral("schema-six-window"));
    CHECK(snapshot->workspace.calendarEventContextMenuLayout.empty());
}

TEST_CASE("settings updates require the current revision and round-trip typed values",
          "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("revisioned.ini"));
    auto repository = repositoryFor(path);
    const auto initialResult = repository.load();
    const auto* initial = std::get_if<SettingsSnapshot>(&initialResult);
    REQUIRE(initial != nullptr);

    auto update = emptyUpdate();
    update.accounts = std::vector<AccountSettings>{AccountSettings{
        .id = QStringLiteral("configured-1"),
        .revision = 1,
        .displayName = QStringLiteral("Work"),
        .sessionUrl = {},
        .loginEmail = QStringLiteral("work@example.test"),
        .tokenEndpoint = QStringLiteral("https://auth.example.test/token"),
        .oauthClientId = QStringLiteral("client-id"),
        .oauthIssuer = QStringLiteral("https://auth.example.test"),
        .oauthResource = QStringLiteral("https://mail.example.test/jmap"),
        .oauthScope = QStringLiteral("mail offline_access"),
        .revocationEndpoint = QStringLiteral("https://auth.example.test/revoke"),
        .registrationClientUri = QStringLiteral("https://auth.example.test/register/client-id"),
        .hasCredentials = false,
        .credentialHandle = {},
        .tokenExpiresAtEpochSeconds = 1'785'784'100,
        .reauthenticationRequired = true,
        .cachedAccountIds = {}}};
    update.syncedMailboxSelections = std::vector<javelin::protocol::MailboxSelectionSettings>{{
        .accountId = QStringLiteral("account-1"),
        .mailboxIds = {},
    }};
    update.notificationMailboxSelections =
        std::vector<javelin::protocol::MailboxSelectionSettings>{{
            .accountId = QStringLiteral("account-1"),
            .mailboxIds = {},
        }};
    update.undoSendDelaySeconds = 45;
    update.undoSendUsesDialog = true;
    update.workspace = javelin::protocol::WorkspaceSettings{
        .formatVersion = 1,
        .mainWindowState = QByteArrayLiteral("workspace-state"),
        .composeRichTextDefault = false,
        .calendarColorOverrides = {{.calendarId = QStringLiteral("calendar-2"),
                                    .color = QStringLiteral("#abcdef")}},
        .emailContextMenuLayout = {QStringLiteral("compose_reply"), QStringLiteral("separator"),
                                   QStringLiteral("archive_email")},
        .calendarEventContextMenuLayout = {QStringLiteral("calendar_event_edit"),
                                           QStringLiteral("separator"),
                                           QStringLiteral("calendar_event_delete")},
    };
    const auto accepted =
        repository.update({.baseRevision = initial->revision, .update = std::move(update)});
    const auto* updated = std::get_if<javelin::protocol::SettingsUpdated>(&accepted);
    REQUIRE(updated != nullptr);
    CHECK(updated->revision.value == 1);

    const auto reloadedResult = repository.load();
    const auto* reloaded = std::get_if<SettingsSnapshot>(&reloadedResult);
    REQUIRE(reloaded != nullptr);
    CHECK(reloaded->revision.value == 1);
    CHECK(reloaded->accounts.front().displayName == QStringLiteral("Work"));
    CHECK_FALSE(reloaded->accounts.front().hasCredentials);
    CHECK(reloaded->accounts.front().credentialHandle.isEmpty());
    CHECK(reloaded->accounts.front().tokenEndpoint ==
          QStringLiteral("https://auth.example.test/token"));
    CHECK(reloaded->accounts.front().oauthClientId == QStringLiteral("client-id"));
    CHECK(reloaded->accounts.front().oauthIssuer == QStringLiteral("https://auth.example.test"));
    CHECK(reloaded->accounts.front().oauthResource ==
          QStringLiteral("https://mail.example.test/jmap"));
    CHECK(reloaded->accounts.front().oauthScope == QStringLiteral("mail offline_access"));
    CHECK(reloaded->accounts.front().revocationEndpoint ==
          QStringLiteral("https://auth.example.test/revoke"));
    CHECK(reloaded->accounts.front().registrationClientUri ==
          QStringLiteral("https://auth.example.test/register/client-id"));
    CHECK(reloaded->accounts.front().tokenExpiresAtEpochSeconds == 1'785'784'100);
    CHECK(reloaded->accounts.front().reauthenticationRequired);
    REQUIRE(reloaded->syncedMailboxSelections.size() == 1);
    CHECK(reloaded->syncedMailboxSelections.front().mailboxIds.empty());
    REQUIRE(reloaded->notificationMailboxSelections.size() == 1);
    CHECK(reloaded->notificationMailboxSelections.front().mailboxIds.empty());
    CHECK(reloaded->undoSendDelaySeconds == 45);
    CHECK(reloaded->undoSendUsesDialog);
    CHECK(reloaded->workspace.mainWindowState == QByteArrayLiteral("workspace-state"));
    CHECK_FALSE(reloaded->workspace.composeRichTextDefault);
    REQUIRE(reloaded->workspace.calendarColorOverrides.size() == 1);
    CHECK(reloaded->workspace.calendarColorOverrides.front().calendarId ==
          QStringLiteral("calendar-2"));
    CHECK(reloaded->workspace.calendarColorOverrides.front().color == QStringLiteral("#abcdef"));
    CHECK(reloaded->workspace.emailContextMenuLayout ==
          std::vector<QString>{QStringLiteral("compose_reply"), QStringLiteral("separator"),
                               QStringLiteral("archive_email")});
    CHECK(reloaded->workspace.calendarEventContextMenuLayout ==
          std::vector<QString>{QStringLiteral("calendar_event_edit"), QStringLiteral("separator"),
                               QStringLiteral("calendar_event_delete")});

    auto staleUpdate = emptyUpdate();
    staleUpdate.appearance = javelin::protocol::AppearanceSettings{.messageColorMode = 1};
    const auto rejected =
        repository.update({.baseRevision = initial->revision, .update = std::move(staleUpdate)});
    const auto* stale = std::get_if<javelin::protocol::SettingsUpdateRejected>(&rejected);
    REQUIRE(stale != nullptr);
    CHECK(stale->currentRevision.value == 1);
    CHECK(stale->error.code == javelin::protocol::BoundaryErrorCode::StaleSettingsRevision);
}

TEST_CASE("settings migration fails closed on malformed account records", "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("malformed.ini"));
    QSettings settings{path, QSettings::IniFormat};
    settings.beginGroup(QStringLiteral("accounts"));
    settings.beginWriteArray(QStringLiteral("size"), 1);
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("loginEmail"), QStringLiteral("user@example.test"));
    settings.endArray();
    settings.endGroup();
    settings.sync();
    REQUIRE(settings.status() == QSettings::NoError);

    auto repository = repositoryFor(path);
    const auto result = repository.load();
    const auto* error = std::get_if<SettingsRepositoryError>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->code == javelin::app::SettingsRepositoryErrorCode::MigrationFailed);
    QSettings unchanged{path, QSettings::IniFormat};
    CHECK_FALSE(unchanged.contains(QStringLiteral("settings/schemaVersion")));
}
