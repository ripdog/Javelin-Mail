#include "app/DaemonProcess.h"
#include "app/CacheLocationProvider.h"
#include "app/DaemonRemoteActionDispatcher.h"
#include "app/DaemonServices.h"
#include "app/MailApplicationService.h"
#include "app/MessageListMaterializationPort.h"
#include "app/RemoteCodec.h"
#include "jmap/cache/Database.h"

#include <QCoroTask>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
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
            .guiExecutable = {},
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

    javelin::app::DaemonProcess duplicate{optionsFor(runtimeDirectory, cacheRoot, settingsPath)};
    const auto duplicateError = duplicate.start();
    REQUIRE(duplicateError.has_value());
    CHECK(duplicateError->code == javelin::app::DaemonStartupErrorCode::InstanceAlreadyRunning);
    CHECK(process.isReady());

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
    CHECK(snapshot->snapshot.schemaVersion == 2);
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
                   .undoSendDelaySeconds = std::nullopt,
                   .workspace = std::nullopt},
    });
    const auto* updated = std::get_if<javelin::protocol::SettingsUpdated>(&update);
    REQUIRE(updated != nullptr);
    CHECK(updated->revision.value == 1);

    CHECK_FALSE(process.handlePing({}).has_value());
    process.enqueueActivation(javelin::protocol::OpenMessageRoute{
        .accountId = QStringLiteral("account-1"),
        .mailboxId = QStringLiteral("mailbox-1"),
        .mailboxName = QStringLiteral("Projects"),
        .threadId = QStringLiteral("thread-1"),
        .emailId = QStringLiteral("email-1"),
        .activationToken = QStringLiteral("notification-token"),
    });
    CHECK(process.pendingActivationCount() == 1);
    const auto activationError = process.handleGuiActivation(
        javelin::protocol::RaiseGuiRoute{.activationToken = QStringLiteral("raise-token")});
    REQUIRE(activationError.has_value());
    CHECK(activationError->code == javelin::protocol::BoundaryErrorCode::Busy);
    CHECK(process.pendingActivationCount() == 1);
    process.stop();
    CHECK_FALSE(process.isReady());
}

TEST_CASE("daemon does not queue vault metadata for undiscovered connection ids",
          "[app][daemon][settings][offline][vault]")
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

    const auto currentSettings = process.handleGetSettings({});
    const auto* current = std::get_if<javelin::protocol::SettingsSnapshotReply>(&currentSettings);
    REQUIRE(current != nullptr);
    const auto update = process.handleUpdateSettings({
        .baseRevision = current->snapshot.revision,
        .update = {.accounts = std::vector{javelin::protocol::AccountSettings{
                       .id = QStringLiteral("connection-1"),
                       .revision = 0,
                       .displayName = QStringLiteral("Example"),
                       .sessionUrl = QStringLiteral("https://example.test/jmap"),
                       .loginEmail = QStringLiteral("user@example.test"),
                       .apiKey = QStringLiteral("secret"),
                       .refreshToken = {},
                       .tokenEndpoint = {},
                       .oauthClientId = {},
                       .tokenExpiresAtEpochSeconds = 0,
                       .cachedAccountIds = {},
                   }},
                   .syncedMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .notificationMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
                   .translation = std::nullopt,
                   .appearance = std::nullopt,
                   .attachments = std::nullopt,
                   .undoSendDelaySeconds = std::nullopt,
                   .workspace = std::nullopt},
    });
    REQUIRE(std::holds_alternative<javelin::protocol::SettingsUpdated>(update));

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("daemon-undiscovered-vault-test"),
        .databasePath = process.databasePath(),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery jobs{connection.database()};
    jobs.prepare(QStringLiteral(
        "SELECT COUNT(*) FROM mail_vault_projection_jobs WHERE account_id=:account"));
    jobs.bindValue(QStringLiteral(":account"), QStringLiteral("connection-1"));
    REQUIRE(jobs.exec());
    REQUIRE(jobs.next());
    CHECK(jobs.value(0).toInt() == 0);

    process.stop();
}

TEST_CASE("daemon applies offline mailbox settings by cached JMAP account id",
          "[app][daemon][settings][offline]")
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

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("daemon-offline-settings-test"),
        .databasePath = process.databasePath(),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                       "VALUES('account-1','user@example.test','https://example.test/jmap',1)")));
    REQUIRE(seed.exec(QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name,role) "
                                     "VALUES('account-1','archive','Archive','archive')")));

    const auto currentSettings = process.handleGetSettings({});
    const auto* current = std::get_if<javelin::protocol::SettingsSnapshotReply>(&currentSettings);
    REQUIRE(current != nullptr);
    const javelin::protocol::AccountSettings account{
        .id = QStringLiteral("connection-1"),
        .revision = 0,
        .displayName = QStringLiteral("Example"),
        .sessionUrl = QStringLiteral("https://example.test/jmap"),
        .loginEmail = QStringLiteral("user@example.test"),
        .apiKey = QStringLiteral("secret"),
        .refreshToken = {},
        .tokenEndpoint = {},
        .oauthClientId = {},
        .tokenExpiresAtEpochSeconds = 0,
        .cachedAccountIds = {QStringLiteral("account-1")},
    };
    const auto enable = process.handleUpdateSettings({
        .baseRevision = current->snapshot.revision,
        .update = {.accounts = std::vector{account},
                   .syncedMailboxSelections =
                       std::vector{javelin::protocol::MailboxSelectionSettings{
                           .accountId = QStringLiteral("account-1"),
                           .mailboxIds = {QStringLiteral("archive")},
                           .configured = true,
                       }},
                   .notificationMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
                   .translation = std::nullopt,
                   .appearance = std::nullopt,
                   .attachments = std::nullopt,
                   .undoSendDelaySeconds = std::nullopt,
                   .workspace = std::nullopt},
    });
    REQUIRE(std::holds_alternative<javelin::protocol::SettingsUpdated>(enable));

    QSqlQuery scope{connection.database()};
    REQUIRE(scope.exec(QStringLiteral(
        "SELECT desired,status FROM offline_mailbox_scopes WHERE account_id='account-1' AND "
        "mailbox_id='archive'")));
    REQUIRE(scope.next());
    CHECK(scope.value(0).toBool());
    CHECK(scope.value(1).toString() == QStringLiteral("pending"));
    scope.finish();

    REQUIRE(scope.exec(QStringLiteral(
        "UPDATE offline_mailbox_scopes SET status='complete',generation=1,"
        "completed_generation=1 WHERE account_id='account-1' AND mailbox_id='archive'")));
    REQUIRE(scope.exec(
        QStringLiteral("UPDATE background_jobs SET status='complete',pause_requested=0 WHERE "
                       "account_id='account-1' AND kind='full_mail_sync'")));

    const auto enabledSettings = process.handleGetSettings({});
    const auto* enabledSnapshot =
        std::get_if<javelin::protocol::SettingsSnapshotReply>(&enabledSettings);
    REQUIRE(enabledSnapshot != nullptr);
    const auto disable = process.handleUpdateSettings({
        .baseRevision = enabledSnapshot->snapshot.revision,
        .update = {.accounts = std::nullopt,
                   .syncedMailboxSelections =
                       std::vector{javelin::protocol::MailboxSelectionSettings{
                           .accountId = QStringLiteral("account-1"),
                           .mailboxIds = {},
                           .configured = true,
                       }},
                   .notificationMailboxSelections = std::nullopt,
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
                   .translation = std::nullopt,
                   .appearance = std::nullopt,
                   .attachments = std::nullopt,
                   .undoSendDelaySeconds = std::nullopt,
                   .workspace = std::nullopt},
    });
    REQUIRE(std::holds_alternative<javelin::protocol::SettingsUpdated>(disable));
    REQUIRE(scope.exec(QStringLiteral(
        "SELECT desired,status FROM offline_mailbox_scopes WHERE account_id='account-1' AND "
        "mailbox_id='archive'")));
    REQUIRE(scope.next());
    CHECK_FALSE(scope.value(0).toBool());
    CHECK(scope.value(1).toString() == QStringLiteral("paused"));
    scope.finish();

    const auto disabledSettings = process.handleGetSettings({});
    const auto* disabledSnapshot =
        std::get_if<javelin::protocol::SettingsSnapshotReply>(&disabledSettings);
    REQUIRE(disabledSnapshot != nullptr);
    const auto reenable = process.handleUpdateSettings({
        .baseRevision = disabledSnapshot->snapshot.revision,
        .update = {.accounts = std::nullopt,
                   .syncedMailboxSelections =
                       std::vector{javelin::protocol::MailboxSelectionSettings{
                           .accountId = QStringLiteral("account-1"),
                           .mailboxIds = {QStringLiteral("archive")},
                           .configured = true,
                       }},
                   .notificationMailboxSelections = std::nullopt,
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
                   .translation = std::nullopt,
                   .appearance = std::nullopt,
                   .attachments = std::nullopt,
                   .undoSendDelaySeconds = std::nullopt,
                   .workspace = std::nullopt},
    });
    REQUIRE(std::holds_alternative<javelin::protocol::SettingsUpdated>(reenable));
    REQUIRE(scope.exec(QStringLiteral(
        "SELECT desired,status FROM offline_mailbox_scopes WHERE account_id='account-1' AND "
        "mailbox_id='archive'")));
    REQUIRE(scope.next());
    CHECK(scope.value(0).toBool());
    CHECK(scope.value(1).toString() == QStringLiteral("complete"));
    scope.finish();

    REQUIRE(scope.exec(QStringLiteral(
        "SELECT status,pause_requested FROM background_jobs WHERE account_id='account-1' AND "
        "kind='full_mail_sync'")));
    REQUIRE(scope.next());
    CHECK(scope.value(0).toString() == QStringLiteral("queued"));
    CHECK_FALSE(scope.value(1).toBool());

    process.stop();
}

TEST_CASE("completed offline mailbox pages materialize without server access",
          "[app][daemon][offline][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());

    const auto locationResult =
        javelin::app::CacheLocationProvider{temporaryDirectory.path()}.loadOrCreate();
    REQUIRE(std::holds_alternative<javelin::app::CacheLocation>(locationResult));
    javelin::app::DaemonServices services{std::get<javelin::app::CacheLocation>(locationResult)};
    auto& connection = services.databaseConnection();
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                       "VALUES('account-1','user@example.test','http://127.0.0.1:9/jmap',1)")));
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO mailboxes(account_id,mailbox_id,name,role,total_emails,total_threads) "
        "VALUES('account-1','archive','Archive','archive',150,150)")));
    REQUIRE(seed.exec(QStringLiteral(
        "WITH RECURSIVE seq(n) AS (VALUES(0) UNION ALL SELECT n+1 FROM seq WHERE n<149) "
        "INSERT INTO emails(account_id,email_id,thread_id,received_at,subject,preview) "
        "SELECT 'account-1',printf('email-%03d',n),printf('thread-%03d',n),"
        "printf('2026-01-01T%02d:%02d:00Z',n/60,n%60),printf('Message %03d',n),'' "
        "FROM seq")));
    REQUIRE(seed.exec(QStringLiteral(
        "WITH RECURSIVE seq(n) AS (VALUES(0) UNION ALL SELECT n+1 FROM seq WHERE n<149) "
        "INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) "
        "SELECT 'account-1',printf('email-%03d',n),'archive' FROM seq")));
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,generation,"
        "completed_generation,query_state) VALUES('account-1','archive',1,'complete',1,1,"
        "'state-current')")));
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO sync_state(account_id,object_type,query_key,state_token) VALUES("
        "'account-1','EmailQuery',"
        "'mailbox:archive|sort:receivedAt:desc|collapseThreads:true','state-current')")));

    services.mailService().applySettings({javelin::app::AccountSyncConfiguration{
        .settings = {.connectionId = "connection-1",
                     .revision = 0,
                     .sessionUrl = "http://127.0.0.1:9/jmap",
                     .loginEmail = "user@example.test",
                     .apiKey = "secret",
                     .refreshToken = {},
                     .tokenEndpoint = {},
                     .oauthClientId = {}},
        .accountId = "account-1",
        .mailboxIds = {"archive"},
        .fullSyncMailboxIds = {"archive"},
        .notificationMailboxIds = {},
        .notificationMailboxSelectionConfigured = false,
    }});

    const auto result = QCoro::waitFor(services.mailService().requestMailboxWindow({
        .accountId = "account-1",
        .mailboxId = "archive",
        .offset = 100,
        .limit = 25,
        .sort = {},
        .forceRefresh = false,
        .anchor = std::nullopt,
        .anchorOffset = 1,
    }));
    const auto* summary = std::get_if<javelin::app::MailboxWindowSummary>(&result);
    REQUIRE(summary != nullptr);
    CHECK(summary->offset == 100);
    CHECK(summary->representativeCount == 25);
    CHECK(summary->total == std::optional<std::size_t>{150});
    CHECK(summary->queryState == "state-current");
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

TEST_CASE("daemon replays completed remote action results", "[app][daemon][ipc][replay]")
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
    REQUIRE_FALSE(process.start().has_value());

    const auto encoded = javelin::app::remote::encode(std::string{"missing-owner"});
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const javelin::protocol::CommandRequest request{
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .kind = javelin::protocol::RemoteActionKind::ContactRequestRefresh,
                .payload = std::get<QByteArray>(encoded),
            },
    };
    const auto admitted = process.handleCommand(request);
    const auto* accepted = std::get_if<javelin::protocol::CommandAccepted>(&admitted);
    REQUIRE(accepted != nullptr);
    REQUIRE(accepted->operation.has_value());
    CHECK(accepted->epoch == process.currentEpoch());
    CHECK(accepted->changedDomains == std::vector{javelin::protocol::ChangedDomain::Contacts});
    CHECK_FALSE(accepted->immediateResult.has_value());

    std::optional<javelin::protocol::CommandAccepted> completed;
    for (int attempt = 0; attempt < 100 && !completed.has_value(); ++attempt)
    {
        QCoreApplication::processEvents();
        const auto replay = process.handleCommand(request);
        const auto* replayed = std::get_if<javelin::protocol::CommandAccepted>(&replay);
        REQUIRE(replayed != nullptr);
        if (replayed->immediateResult.has_value())
            completed = *replayed;
        else
            QThread::msleep(1);
    }
    REQUIRE(completed.has_value());
    CHECK_FALSE(completed->operation.has_value());
    CHECK(completed->epoch == process.currentEpoch());
    CHECK(completed->changedDomains == std::vector{javelin::protocol::ChangedDomain::Contacts});
}

TEST_CASE("daemon retains command UUID replay protection after GUI resources are released",
          "[app][daemon][ipc][replay]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDirectory;
    REQUIRE(temporaryDirectory.isValid());
    const auto cacheRoot = temporaryDirectory.filePath(QStringLiteral("cache"));
    REQUIRE(QDir{}.mkpath(cacheRoot));

    auto locationResult = javelin::app::CacheLocationProvider{cacheRoot}.loadOrCreate();
    REQUIRE(std::holds_alternative<javelin::app::CacheLocation>(locationResult));
    javelin::app::DaemonServices services{
        std::get<javelin::app::CacheLocation>(std::move(locationResult))};
    const javelin::protocol::InvalidationEpoch epoch{.value = 42};

    struct EventSink final : javelin::protocol::BoundaryEventSink
    {
        void onBoundaryEvent(const javelin::protocol::BoundaryEvent&) override
        {
        }
    } eventSink;
    javelin::app::DaemonRemoteActionDispatcher dispatcher{
        services, eventSink, [epoch] { return epoch; },
        []() -> std::optional<javelin::protocol::BoundaryError> { return std::nullopt; }};

    const javelin::protocol::CommandId commandId{.value = QUuid::createUuid()};
    const auto first = dispatcher.dispatch({
        .id = commandId,
        .command =
            javelin::protocol::RemoteActionCommand{
                .kind = javelin::protocol::RemoteActionKind::WorkSummary,
                .payload = {},
            },
    });
    const auto* summaryAccepted = std::get_if<javelin::protocol::CommandAccepted>(&first);
    REQUIRE(summaryAccepted != nullptr);
    CHECK(summaryAccepted->epoch == epoch);
    CHECK(summaryAccepted->changedDomains.empty());

    const auto jobId = javelin::app::remote::encode(std::string{"missing-job"});
    REQUIRE(std::holds_alternative<QByteArray>(jobId));
    const auto workMutation = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .kind = javelin::protocol::RemoteActionKind::WorkRetry,
                .payload = std::get<QByteArray>(jobId),
            },
    });
    const auto* workAccepted = std::get_if<javelin::protocol::CommandAccepted>(&workMutation);
    REQUIRE(workAccepted != nullptr);
    CHECK(workAccepted->epoch == epoch);
    CHECK(workAccepted->changedDomains ==
          std::vector{javelin::protocol::ChangedDomain::BackgroundJobs});

    dispatcher.releaseGuiResources();
    const auto reused = dispatcher.dispatch({
        .id = commandId,
        .command =
            javelin::protocol::RemoteActionCommand{
                .kind = javelin::protocol::RemoteActionKind::WorkList,
                .payload = {},
            },
    });
    const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reused);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == javelin::protocol::BoundaryErrorCode::InvalidRequest);
}
