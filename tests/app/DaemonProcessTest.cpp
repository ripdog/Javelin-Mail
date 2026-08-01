#include "app/DaemonProcess.h"

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <memory>
#include <variant>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char applicationName[] = "javelin-daemon-tests";
            static char* argv[] = {applicationName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] javelin::app::DaemonProcessOptions optionsFor(const QString& runtimeDirectory,
                                                                const QString& cacheRoot,
                                                                const QString& settingsPath)
    {
        return {
            .socket = {.runtimeDirectory = runtimeDirectory,
                       .socketPath = runtimeDirectory + QStringLiteral("/javelind.sock"),
                       .limits = {},
                       .protocol = {.major = 1, .minor = 0},
                       .expectedBuild = std::nullopt,
                       .maximumQueuedFrames = 16,
                       .maximumQueuedBytes = 4096,
                       .responseTimeoutMilliseconds = 1000,
                       .enforcePeerCredentials = false},
            .protocol = {.major = 1, .minor = 0},
            .build = {.application = QStringLiteral("Javelin-Mail"),
                      .revision = QStringLiteral("daemon-test")},
            .cacheRootPath = cacheRoot,
            .settingsPath = settingsPath,
        };
    }
} // namespace

TEST_CASE("daemon process migrates settings before exposing readiness", "[app][daemon]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());

    const auto runtimeDirectory = temporaryDirectory.filePath(QStringLiteral("runtime"));
    const auto cacheRoot = temporaryDirectory.filePath(QStringLiteral("cache"));
    const auto settingsPath = temporaryDirectory.filePath(QStringLiteral("settings.ini"));
    REQUIRE(QDir{}.mkpath(runtimeDirectory));
    REQUIRE(QDir{}.mkpath(cacheRoot));
    REQUIRE(QFile::setPermissions(runtimeDirectory, QFileDevice::ReadOwner |
                                                        QFileDevice::WriteOwner |
                                                        QFileDevice::ExeOwner));

    javelin::app::DaemonProcess process{optionsFor(runtimeDirectory, cacheRoot, settingsPath)};
    const auto startupError = process.start();
    INFO((startupError.has_value() ? startupError->detail.toStdString() : std::string{"no error"}));
    REQUIRE_FALSE(startupError.has_value());
    CHECK(process.isReady());
    CHECK_FALSE(process.hasGuiConnection());
    CHECK_FALSE(process.databasePath().isEmpty());

    const auto hello = process.handleHello({.protocol = {.major = 1, .minor = 0},
                                            .build = {.application = QStringLiteral("Javelin-Mail"),
                                                      .revision = QStringLiteral("daemon-test")}});
    const auto* ready = std::get_if<javelin::protocol::ReadyReply>(&hello);
    REQUIRE(ready != nullptr);
    CHECK(ready->protocol.major == 1);
    CHECK(ready->cache.instance.value != QUuid{});
    CHECK(ready->cache.schema.value > 0);

    const auto settings = process.handleGetSettings({});
    const auto* snapshot = std::get_if<javelin::protocol::SettingsSnapshotReply>(&settings);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot.schemaVersion == 1);
    CHECK(snapshot->snapshot.revision.value == 0);

    const auto update = process.handleUpdateSettings({
        .baseRevision = snapshot->snapshot.revision,
        .update = {.accounts = std::nullopt,
                   .syncedMailboxSelections = std::nullopt,
                   .notificationMailboxSelections = std::nullopt,
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
                   .translation =
                       javelin::protocol::TranslationSettings{
                           .enabled = true,
                           .apiKeyOverride = {},
                           .targetLanguage = QStringLiteral("mi-NZ"),
                           .autoTranslateSenders = {},
                           .autoTranslateDomains = {},
                       },
                   .appearance = std::nullopt,
                   .attachments = std::nullopt,
                   .undoSendDelaySeconds = std::nullopt},
    });
    const auto* updated = std::get_if<javelin::protocol::SettingsUpdated>(&update);
    REQUIRE(updated != nullptr);
    CHECK(updated->revision.value == 1);

    CHECK_FALSE(process.handlePing({}).has_value());
    process.stop();
    CHECK_FALSE(process.isReady());
}

TEST_CASE("daemon rejects requests after shutdown without pretending to be offline",
          "[app][daemon]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto runtimeDirectory = temporaryDirectory.filePath(QStringLiteral("runtime"));
    const auto cacheRoot = temporaryDirectory.filePath(QStringLiteral("cache"));
    REQUIRE(QDir{}.mkpath(runtimeDirectory));
    REQUIRE(QDir{}.mkpath(cacheRoot));
    REQUIRE(QFile::setPermissions(runtimeDirectory, QFileDevice::ReadOwner |
                                                        QFileDevice::WriteOwner |
                                                        QFileDevice::ExeOwner));

    javelin::app::DaemonProcess process{optionsFor(
        runtimeDirectory, cacheRoot, temporaryDirectory.filePath(QStringLiteral("settings.ini")))};
    const auto startupError = process.start();
    INFO((startupError.has_value() ? startupError->detail.toStdString() : std::string{"no error"}));
    REQUIRE_FALSE(startupError.has_value());
    process.stop();

    const auto reply = process.handleCommand({.id = {.value = QUuid::createUuid()},
                                              .command = javelin::protocol::RefreshAccountCommand{
                                                  .accountId = QStringLiteral("account-1"),
                                                  .force = true,
                                              }});
    const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == javelin::protocol::BoundaryErrorCode::DaemonShuttingDown);
}
