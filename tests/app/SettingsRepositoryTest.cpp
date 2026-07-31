#include "app/SettingsRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <QSettings>
#include <QTemporaryDir>

#include <memory>
#include <utility>
#include <variant>

namespace
{

    using javelin::app::SettingsReadResult;
    using javelin::app::SettingsRepository;
    using javelin::app::SettingsRepositoryError;
    using javelin::protocol::AccountSettings;
    using javelin::protocol::SettingsSnapshot;
    using javelin::protocol::SettingsUpdate;
    using javelin::protocol::TranslationSettings;

    [[nodiscard]] SettingsRepository repositoryFor(const QString& path)
    {
        return SettingsRepository{std::make_unique<QSettings>(path, QSettings::IniFormat)};
    }

    [[nodiscard]] SettingsUpdate emptyUpdate()
    {
        return {.accounts = std::nullopt,
                .syncedMailboxSelections = std::nullopt,
                .notificationMailboxSelections = std::nullopt,
                .remoteContentSenders = std::nullopt,
                .remoteContentDomains = std::nullopt,
                .translation = std::nullopt,
                .appearance = std::nullopt,
                .attachments = std::nullopt,
                .undoSendDelaySeconds = std::nullopt};
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
    CHECK(snapshot->schemaVersion == 1);
    CHECK(snapshot->translation.targetLanguage == QStringLiteral("en"));
    CHECK(snapshot->undoSendDelaySeconds == 10);

    QSettings persisted{path, QSettings::IniFormat};
    CHECK(persisted.value(QStringLiteral("settings/schemaVersion")).toUInt() == 1);
    CHECK(persisted.value(QStringLiteral("settings/revision")).toULongLong() == 0);
}

TEST_CASE("settings repository migrates the complete legacy operational shape", "[app][settings]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("legacy.ini"));
    writeLegacySettings(path);

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
                          .apiKey = QStringLiteral("secret"),
                          .cachedAccountIds = {QStringLiteral("account-1")}});
    REQUIRE(snapshot->syncedMailboxSelections.size() == 1);
    CHECK(snapshot->syncedMailboxSelections.front().mailboxIds ==
          std::vector<QString>{QStringLiteral("inbox"), QStringLiteral("archive")});
    REQUIRE(snapshot->notificationMailboxSelections.size() == 1);
    CHECK(snapshot->notificationMailboxSelections.front().configured);
    CHECK_FALSE(snapshot->translation.enabled);
    CHECK(snapshot->translation.apiKeyOverride == QStringLiteral("translation-key"));
    CHECK(snapshot->translation.targetLanguage == QStringLiteral("ja"));
    CHECK(snapshot->translation.autoTranslateSenders ==
          std::vector<QString>{QStringLiteral("sender@example.test")});
    CHECK(snapshot->translation.autoTranslateDomains ==
          std::vector<QString>{QStringLiteral("example.test")});
    CHECK(snapshot->appearance.messageColorMode == 2);
    CHECK_FALSE(snapshot->attachments.alwaysAsk);
    CHECK(snapshot->attachments.directory == QStringLiteral("/tmp/mail"));
    CHECK(snapshot->undoSendDelaySeconds == 30);

    QSettings migrated{path, QSettings::IniFormat};
    CHECK(migrated.value(QStringLiteral("settings/schemaVersion")).toUInt() == 1);
    CHECK(migrated.value(QStringLiteral("settings/revision")).toULongLong() == 0);
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
    update.accounts = std::vector<AccountSettings>{
        AccountSettings{.id = QStringLiteral("configured-1"),
                        .revision = 1,
                        .displayName = QStringLiteral("Work"),
                        .sessionUrl = {},
                        .loginEmail = QStringLiteral("work@example.test"),
                        .apiKey = QStringLiteral("key"),
                        .cachedAccountIds = {}}};
    update.translation = TranslationSettings{.enabled = false,
                                             .apiKeyOverride = QStringLiteral(" key "),
                                             .targetLanguage = QStringLiteral("DE"),
                                             .autoTranslateSenders = {},
                                             .autoTranslateDomains = {}};
    update.undoSendDelaySeconds = 45;
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
    CHECK_FALSE(reloaded->translation.enabled);
    CHECK(reloaded->translation.apiKeyOverride == QStringLiteral("key"));
    CHECK(reloaded->translation.targetLanguage == QStringLiteral("de"));
    CHECK(reloaded->undoSendDelaySeconds == 45);

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
