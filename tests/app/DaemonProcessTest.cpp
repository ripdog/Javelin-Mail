#include "daemon/DaemonProcess.h"
#include "app/AccountRuntimeManager.h"
#include "app/CacheLocationProvider.h"
#include "app/LogStore.h"
#include "app/MailMutationApplicationService.h"
#include "app/MailQueryApplicationService.h"
#include "app/MailboxTreeCacheRead.h"
#include "app/MessageListMaterializationPort.h"
#include "app/RemoteCodec.h"
#include "app/undo/UndoManager.h"
#include "daemon/DaemonServices.h"
#include "daemon/actions/DaemonRemoteActionDispatcher.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxMessageReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/QueryWindowReadRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"
#include "protocol/actions/ActionCatalog.h"
#include "storage/sqlite/DatabaseConnection.h"

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

    template <typename Action, typename... Arguments>
    [[nodiscard]] QByteArray actionPayload(const Arguments&... arguments)
    {
        auto encoded =
            javelin::app::remote::encodeVersioned<Action::requestSchemaVersion>(arguments...);
        REQUIRE(std::holds_alternative<QByteArray>(encoded));
        return std::get<QByteArray>(std::move(encoded));
    }

    [[nodiscard]] std::shared_ptr<javelin::app::MemoryAccountCredentialStore> testCredentialStore()
    {
        static auto store = std::make_shared<javelin::app::MemoryAccountCredentialStore>();
        return store;
    }

    [[nodiscard]] javelin::app::DaemonProcessOptions optionsFor(const QString& runtimeDirectory,
                                                                const QString& cacheRoot,
                                                                const QString& settingsPath)
    {
        return {
            .socket = {.runtimeDirectory = runtimeDirectory,
                       .socketPath = runtimeDirectory + QStringLiteral("/javelind.sock"),
                       .limits = {},
                       .protocol = {.major = 5, .minor = 0},
                       .expectedBuild = std::nullopt,
                       .maximumQueuedFrames = 16,
                       .maximumQueuedBytes = 4096,
                       .responseTimeoutMilliseconds = 1000,
                       .enforcePeerCredentials = false},
            .protocol = {.major = 5, .minor = 0},
            .build = {.application = QStringLiteral("Javelin-Mail"),
                      .revision = QStringLiteral("daemon-test")},
            .guiExecutable = {},
            .cacheRootPath = cacheRoot,
            .settingsPath = settingsPath,
            .credentialStore = testCredentialStore(),
            .enableNetworkReachability = false,
        };
    }
} // namespace

TEST_CASE("daemon log store enforces a bounded history", "[app][daemon][logging]")
{
    auto& logs = javelin::app::LogStore::instance();
    logs.clear();
    logs.setMaximumEntries(3);
    for (int index = 0; index < 5; ++index)
    {
        logs.append({.timestamp = QDateTime::currentDateTime(),
                     .level = QtInfoMsg,
                     .subsystem = QStringLiteral("test"),
                     .message = QString::number(index)});
    }
    const auto entries = logs.entries();
    REQUIRE(entries.size() == 3);
    CHECK(entries.at(0).message == QStringLiteral("2"));
    CHECK(entries.at(2).message == QStringLiteral("4"));
    logs.clear();
    logs.setMaximumEntries(10000);
}

TEST_CASE("daemon log subscription publishes history and live entries", "[app][daemon][logging]")
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

    struct EventSink final : javelin::protocol::BoundaryEventSink
    {
        void onBoundaryEvent(const javelin::protocol::BoundaryEvent& event) override
        {
            if (const auto* logs = std::get_if<javelin::protocol::DaemonLogEntries>(&event))
                events.push_back(*logs);
        }
        std::vector<javelin::protocol::DaemonLogEntries> events;
    } eventSink;

    javelin::app::DaemonRemoteActionDispatcher dispatcher{
        services,
        eventSink,
        [] { return javelin::protocol::InvalidationEpoch{.value = 1}; },
        []() -> std::optional<javelin::protocol::BoundaryError> { return std::nullopt; },
        [](javelin::app::AccountAuthenticationResult result) { return result; },
        [](javelin::app::AccountConnectionSettings settings)
            -> std::variant<javelin::app::AccountConnectionSettings, QString> { return settings; },
        [](javelin::app::OAuthRevocationRequest request)
            -> std::variant<javelin::app::OAuthRevocationRequest, QString> { return request; }};

    auto& store = javelin::app::LogStore::instance();
    store.clear();
    store.setMaximumEntries(1000);
    store.append({.timestamp = QDateTime::fromMSecsSinceEpoch(10),
                  .level = QtInfoMsg,
                  .subsystem = QStringLiteral("daemon.test"),
                  .message = QStringLiteral("historical")});

    const auto subscribePayload =
        actionPayload<javelin::protocol::actions::DeveloperLogSetSubscribed>(true);
    const auto subscribe = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::DeveloperLogSetSubscribed::id,
                .payload = subscribePayload,
            },
    });
    REQUIRE(std::holds_alternative<javelin::protocol::CommandAccepted>(subscribe));
    REQUIRE(eventSink.events.size() == 1);
    REQUIRE(eventSink.events.front().entries.size() == 1);
    CHECK(eventSink.events.front().entries.front().message == QStringLiteral("historical"));

    store.append({.timestamp = QDateTime::fromMSecsSinceEpoch(20),
                  .level = QtWarningMsg,
                  .subsystem = QStringLiteral("daemon.test"),
                  .message = QStringLiteral("live")});
    REQUIRE(eventSink.events.size() == 2);
    REQUIRE(eventSink.events.back().entries.size() == 1);
    CHECK(eventSink.events.back().entries.front().message == QStringLiteral("live"));

    const auto unsubscribePayload =
        actionPayload<javelin::protocol::actions::DeveloperLogSetSubscribed>(false);
    const auto unsubscribe = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::DeveloperLogSetSubscribed::id,
                .payload = unsubscribePayload,
            },
    });
    REQUIRE(std::holds_alternative<javelin::protocol::CommandAccepted>(unsubscribe));
    store.append({.timestamp = QDateTime::fromMSecsSinceEpoch(30),
                  .level = QtInfoMsg,
                  .subsystem = QStringLiteral("daemon.test"),
                  .message = QStringLiteral("after unsubscribe")});
    CHECK(eventSink.events.size() == 2);
    store.clear();
}

TEST_CASE("account bootstrap hydrates credentials before reaching the JMAP service",
          "[app][daemon][auth]")
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

    struct EventSink final : javelin::protocol::BoundaryEventSink
    {
        void onBoundaryEvent(const javelin::protocol::BoundaryEvent&) override
        {
        }
    } eventSink;

    javelin::app::DaemonRemoteActionDispatcher dispatcher{
        services,
        eventSink,
        [] { return javelin::protocol::InvalidationEpoch{.value = 1}; },
        []() -> std::optional<javelin::protocol::BoundaryError> { return std::nullopt; },
        [](javelin::app::AccountAuthenticationResult result) { return result; },
        [](javelin::app::AccountConnectionSettings)
            -> std::variant<javelin::app::AccountConnectionSettings, QString>
        { return QStringLiteral("credential hydration sentinel"); },
        [](javelin::app::OAuthRevocationRequest request)
            -> std::variant<javelin::app::OAuthRevocationRequest, QString> { return request; }};

    const javelin::app::AccountBootstrapIntent intent{
        .settings = {.connectionId = "connection",
                     .revision = 0,
                     .displayName = {},
                     .sessionUrl = "https://mail.example.com/.well-known/jmap",
                     .loginEmail = "alice@example.com",
                     .apiKey = {},
                     .refreshToken = {},
                     .tokenEndpoint = {},
                     .oauthClientId = {}},
        .mailboxIds = {},
    };
    const auto payload = actionPayload<javelin::protocol::actions::AccountBootstrap>(intent);

    const auto reply = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::AccountBootstrap::id,
                .payload = payload,
            },
    });
    const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.detail == QStringLiteral("credential hydration sentinel"));
}

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

    const auto hello = process.handleHello({.protocol = {.major = 5, .minor = 0},
                                            .build = {.application = QStringLiteral("Javelin-Mail"),
                                                      .revision = QStringLiteral("daemon-test")}});
    const auto* ready = std::get_if<javelin::protocol::ReadyReply>(&hello);
    REQUIRE(ready != nullptr);
    CHECK(ready->protocol.major == 5);
    CHECK(ready->cache.instance.value != QUuid{});
    CHECK(ready->cache.schema.value > 0);

    const auto settings = process.handleGetSettings({});
    const auto* snapshot = std::get_if<javelin::protocol::SettingsSnapshotReply>(&settings);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot.schemaVersion == 5);
    CHECK(snapshot->snapshot.revision.value == 0);

    const auto update = process.handleUpdateSettings({
        .baseRevision = snapshot->snapshot.revision,
        .update = {.accounts = std::nullopt,
                   .syncedMailboxSelections = std::nullopt,
                   .notificationMailboxSelections = std::nullopt,
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
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

TEST_CASE("daemon does not relaunch an auto-started GUI after bootstrap rejection",
          "[app][daemon][activation][startup]")
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

    auto options = optionsFor(runtimeDirectory, cacheRoot, settingsPath);
    options.guiExecutable = QString::fromUtf8(JAVELIN_INCOMPATIBLE_GUI_HELPER_PATH);
    javelin::app::DaemonProcess process{std::move(options)};
    REQUIRE_FALSE(process.start().has_value());

    process.enqueueActivation(
        javelin::protocol::RaiseGuiRoute{.activationToken = QStringLiteral("test-token")});
    const auto launchesPath =
        QDir{runtimeDirectory}.filePath(QStringLiteral("incompatible-gui-launches"));

    QByteArray launches;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        QCoreApplication::processEvents();
        QFile file{launchesPath};
        if (file.open(QIODevice::ReadOnly))
            launches = file.readAll();
        if (launches.count('\n') >= 1 && !process.hasGuiConnection())
            break;
        QThread::msleep(5);
    }
    REQUIRE(launches.count('\n') == 1);
    REQUIRE(process.pendingActivationCount() == 1);

    for (int attempt = 0; attempt < 100; ++attempt)
    {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    QFile file{launchesPath};
    REQUIRE(file.open(QIODevice::ReadOnly));
    launches = file.readAll();
    CHECK(launches.count('\n') == 1);
    CHECK(process.pendingActivationCount() == 1);
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

    REQUIRE_FALSE(
        testCredentialStore()
            ->store(QStringLiteral("connection-1"), {.accessToken = QStringLiteral("secret"),
                                                     .refreshToken = {},
                                                     .registrationAccessToken = {}})
            .has_value());
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
                       .tokenEndpoint = {},
                       .oauthClientId = {},
                       .hasCredentials = true,
                       .credentialHandle = {},
                       .tokenExpiresAtEpochSeconds = 0,
                       .cachedAccountIds = {},
                   }},
                   .syncedMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .notificationMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
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
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
        "VALUES('account-1','user@example.test','https://example.test/jmap',1,1)")));
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name,role,is_subscribed) "
                       "VALUES('account-1','archive','Archive','archive',1)")));

    REQUIRE_FALSE(
        testCredentialStore()
            ->store(QStringLiteral("connection-1"), {.accessToken = QStringLiteral("secret"),
                                                     .refreshToken = {},
                                                     .registrationAccessToken = {}})
            .has_value());
    const auto currentSettings = process.handleGetSettings({});
    const auto* current = std::get_if<javelin::protocol::SettingsSnapshotReply>(&currentSettings);
    REQUIRE(current != nullptr);
    const javelin::protocol::AccountSettings account{
        .id = QStringLiteral("connection-1"),
        .revision = 0,
        .displayName = QStringLiteral("Example"),
        .sessionUrl = QStringLiteral("https://example.test/jmap"),
        .loginEmail = QStringLiteral("user@example.test"),
        .tokenEndpoint = {},
        .oauthClientId = {},
        .hasCredentials = true,
        .credentialHandle = {},
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
                       }},
                   .notificationMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
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
                       }},
                   .notificationMailboxSelections = std::nullopt,
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
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
                       }},
                   .notificationMailboxSelections = std::nullopt,
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
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

    services.accountRuntimeManager().applySettings({javelin::app::AccountSyncConfiguration{
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
    }});

    const auto result = QCoro::waitFor(services.mailQueryApplicationService().requestMailboxWindow({
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

TEST_CASE("collapsed Thread archive uses cached source members without Thread refetch",
          "[app][daemon][mail][optimistic][thread-coverage]")
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
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
        "VALUES('account-1','user@example.test','http://127.0.0.1:9/jmap',1,1)")));

    const javelin::jmap::domain::MailboxRights rights{
        .mayReadItems = true,
        .mayAddItems = true,
        .mayRemoveItems = true,
        .maySetSeen = true,
        .maySetKeywords = true,
    };
    javelin::jmap::domain::Mailbox inbox;
    inbox.id = "inbox";
    inbox.name = "Inbox";
    inbox.role = "inbox";
    inbox.totalEmails = 3;
    inbox.totalThreads = 1;
    inbox.isSubscribed = true;
    inbox.myRights = rights;
    javelin::jmap::domain::Mailbox archive;
    archive.id = "archive";
    archive.name = "Archive";
    archive.role = "archive";
    archive.isSubscribed = true;
    archive.myRights = rights;
    javelin::jmap::cache::MailboxRepository mailboxes{connection};
    REQUIRE_FALSE(mailboxes.replaceAll("account-1", {inbox, archive}).has_value());

    javelin::jmap::domain::Email email;
    email.id = "email-1";
    email.threadId = "thread-1";
    email.mailboxIds = {"inbox"};
    email.receivedAt = "2026-08-06T04:00:00Z";
    email.subject = "Projected message";
    email.preview = "Projection test";
    auto child = email;
    child.id = "email-2";
    child.receivedAt = "2026-08-06T03:00:00Z";
    auto secondChild = email;
    secondChild.id = "email-3";
    secondChild.receivedAt = "2026-08-06T02:00:00Z";
    javelin::jmap::cache::EmailRepository emails{connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {email, child, secondChild}).has_value());
    javelin::jmap::cache::ThreadRepository threads{connection};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1", {{.id = "thread-1",
                                                 .emailIds = {"email-1", "email-2", "email-3"}}})
                      .has_value());

    const auto inboxQueryKey = javelin::jmap::sync::mailboxQueryKey({
        .mailboxId = "inbox",
        .sortProperty = "receivedAt",
        .isAscending = false,
        .collapseThreads = true,
    });
    const auto archiveQueryKey = javelin::jmap::sync::mailboxQueryKey({
        .mailboxId = "archive",
        .sortProperty = "receivedAt",
        .isAscending = false,
        .collapseThreads = true,
    });
    javelin::jmap::cache::MailboxWindowRepository windows{connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "inbox",
                          .queryKey = inboxQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 1,
                          .total = 1,
                          .queryState = "query-state-1",
                          .emailIds = {"email-1"},
                      })
                      .has_value());
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "archive",
                          .queryKey = archiveQueryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 0,
                          .total = 0,
                          .queryState = "query-state-1",
                          .emailIds = {},
                      })
                      .has_value());

    // A mailbox-scoped collapsed Thread must not need a synchronous Thread/get. Even with
    // normalized membership stale, the source mailbox projection already identifies its members.
    REQUIRE_FALSE(threads.markStale("account-1", std::vector<std::string>{"thread-1"}).has_value());

    std::vector<javelin::app::MailCacheChange> cacheChanges;
    QObject::connect(&services.mailMutationApplicationService(),
                     &javelin::app::MailMutationApplicationService::cacheCommitted,
                     &services.mailMutationApplicationService(),
                     [&cacheChanges](javelin::app::MailCacheChange change)
                     { cacheChanges.push_back(std::move(change)); });

    const auto queued =
        QCoro::waitFor(services.mailMutationApplicationService().queueMailboxSelectionMutation({
            .accountId = "account-1",
            .selection = {javelin::app::SelectedCollapsedThread{
                .threadId = "thread-1",
            }},
            .operation = javelin::app::MailboxSelectionOperation::Archive,
            .sourceMailboxId = "inbox",
            .destinationMailboxId = std::nullopt,
        }));
    const auto* summary = std::get_if<javelin::app::QueuedMailboxSelectionMutation>(&queued);
    REQUIRE(summary != nullptr);
    CHECK(summary->queuedEmailCount == 3);
    CHECK(summary->queuedMutations.size() == 3);
    REQUIRE(summary->historyEntryId.has_value());
    const auto historyEntry =
        std::ranges::find(services.undoManager().entries(), *summary->historyEntryId,
                          &javelin::app::undo::HistoryEntry::entryId);
    REQUIRE(historyEntry != services.undoManager().entries().end());
    const auto* archiveHistory =
        std::get_if<javelin::app::undo::MailPatchHistory>(&historyEntry->payload);
    REQUIRE(archiveHistory != nullptr);
    REQUIRE(archiveHistory->items.size() == 3);
    CHECK(archiveHistory->items[0].inverse.addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(archiveHistory->items[1].inverse.addMailboxIds == std::vector<std::string>{"inbox"});
    CHECK(archiveHistory->items[2].inverse.addMailboxIds == std::vector<std::string>{"inbox"});
    REQUIRE(cacheChanges.size() == 1);
    CHECK(cacheChanges.front().optimisticProjection);
    CHECK(cacheChanges.front().mailboxIds.size() == 2);
    CHECK(cacheChanges.front().mailboxIds.contains(QStringLiteral("inbox")));
    CHECK(cacheChanges.front().mailboxIds.contains(QStringLiteral("archive")));

    javelin::jmap::cache::MailboxMessageReadRepository mailboxMessages{connection};
    javelin::jmap::cache::QueryWindowReadRepository queryWindows{connection, mailboxMessages};
    const auto inboxPageResult =
        queryWindows.loadMailboxWindow("account-1", inboxQueryKey, 0, 100, {});
    const auto* inboxPage =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&inboxPageResult);
    REQUIRE(inboxPage != nullptr);
    REQUIRE(inboxPage->has_value());
    CHECK((*inboxPage)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    CHECK((*inboxPage)->items.empty());

    const auto archivePageResult =
        queryWindows.loadMailboxWindow("account-1", archiveQueryKey, 0, 100, {});
    const auto* archivePage =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&archivePageResult);
    REQUIRE(archivePage != nullptr);
    REQUIRE(archivePage->has_value());
    CHECK((*archivePage)->coverage == javelin::jmap::cache::QueryWindowCoverage::LocallyProjected);
    REQUIRE((*archivePage)->items.size() == 1);
    CHECK((*archivePage)->items.front().emailId == "email-1");

    services.accountRuntimeManager().applySettings({javelin::app::AccountSyncConfiguration{
        .settings = {.connectionId = "connection-1",
                     .revision = 0,
                     .sessionUrl = "http://127.0.0.1:9/jmap",
                     .loginEmail = "user@example.test",
                     .apiKey = "secret",
                     .refreshToken = {},
                     .tokenEndpoint = {},
                     .oauthClientId = {}},
        .accountId = "account-1",
        .mailboxIds = {"inbox", "archive"},
        .fullSyncMailboxIds = {},
        .notificationMailboxIds = {},
    }});

    const auto materialized =
        QCoro::waitFor(services.mailQueryApplicationService().requestMailboxWindow({
            .accountId = "account-1",
            .mailboxId = "archive",
            .offset = 0,
            .limit = 100,
            .sort = {},
            .forceRefresh = false,
            .anchor = std::nullopt,
            .anchorOffset = 1,
        }));
    const auto* materializedSummary =
        std::get_if<javelin::app::MailboxWindowSummary>(&materialized);
    REQUIRE(materializedSummary != nullptr);
    CHECK(materializedSummary->representativeCount == 1);
    CHECK(materializedSummary->total == std::optional<std::size_t>{1});

    REQUIRE_FALSE(threads.markStale("account-1", std::vector<std::string>{"thread-1"}).has_value());
    QSqlQuery offlineScope{connection.database()};
    REQUIRE(offlineScope.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,generation,"
        "completed_generation) VALUES('account-1','archive',1,'complete',3,3)")));
    const auto offlineFlagged =
        QCoro::waitFor(services.mailMutationApplicationService().queueSetMessagesFlagged(
            "account-1", "archive",
            {javelin::app::SelectedCollapsedThread{
                .threadId = "thread-1",
            }},
            true));
    const auto* offlineSummary =
        std::get_if<javelin::app::QueuedMessageSelectionMutation>(&offlineFlagged);
    REQUIRE(offlineSummary != nullptr);
    CHECK(offlineSummary->queuedEmailCount == 3);

    const auto unavailable =
        QCoro::waitFor(services.mailMutationApplicationService().queueSetMessagesFlagged(
            "account-1", std::nullopt,
            {javelin::app::SelectedCollapsedThread{
                .threadId = "thread-1",
            }},
            true));
    CHECK(std::holds_alternative<javelin::jmap::OperationError>(unavailable));
    QSqlQuery mutationCount{connection.database()};
    REQUIRE(mutationCount.exec(QStringLiteral("SELECT COUNT(*) FROM mutation_journal")));
    REQUIRE(mutationCount.next());
    CHECK(mutationCount.value(0).toInt() == 6);
}

TEST_CASE("Thread materialization invalidates affected windows once per batch",
          "[app][daemon][mail][thread-coverage]")
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
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id) VALUES"
                       "('account-1','email-a','thread-a'),('account-1','email-b','thread-b'),"
                       "('account-1','email-c','thread-c')")));

    javelin::jmap::cache::MailboxWindowRepository mailboxWindows{connection};
    REQUIRE_FALSE(mailboxWindows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "affected-mailbox",
                          .queryKey = "affected-mailbox-query",
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 2,
                          .total = 2,
                          .queryState = "mailbox-state",
                          .emailIds = {"email-a", "email-b"},
                      })
                      .has_value());
    REQUIRE_FALSE(mailboxWindows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "unaffected-mailbox",
                          .queryKey = "unaffected-mailbox-query",
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 1,
                          .total = 1,
                          .queryState = "mailbox-state",
                          .emailIds = {"email-c"},
                      })
                      .has_value());

    javelin::jmap::cache::SearchWindowRepository searchWindows{connection};
    REQUIRE_FALSE(searchWindows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "affected-search",
                          .offset = 25,
                          .limit = 25,
                          .position = 25,
                          .returnedLimit = 2,
                          .total = 50,
                          .queryState = "search-state",
                          .emailIds = {"email-a", "email-b"},
                      })
                      .has_value());
    REQUIRE_FALSE(searchWindows
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "unaffected-search",
                          .offset = 0,
                          .limit = 25,
                          .position = 0,
                          .returnedLimit = 1,
                          .total = 1,
                          .queryState = "search-state",
                          .emailIds = {"email-c"},
                      })
                      .has_value());

    std::vector<javelin::app::MailCacheChange> changes;
    QObject::connect(&services.mailQueryApplicationService(),
                     &javelin::app::MailQueryApplicationService::cacheCommitted,
                     &services.mailQueryApplicationService(),
                     [&changes](javelin::app::MailCacheChange change)
                     { changes.push_back(std::move(change)); });

    services.mailQueryApplicationService().publishThreadMaterializationCommitted(
        QStringLiteral("account-1"), {});
    CHECK(changes.empty());
    services.mailQueryApplicationService().publishThreadMaterializationCommitted(
        QStringLiteral("account-1"), {QStringLiteral("thread-a"), QStringLiteral("thread-b")});

    REQUIRE(changes.size() == 1);
    REQUIRE(changes.front().queryWindows.size() == 1);
    CHECK(changes.front().queryWindows.front().mailboxId == QStringLiteral("affected-mailbox"));
    CHECK(changes.front().queryWindows.front().offset == 0);
    CHECK(changes.front().queryWindows.front().limit == 100);
    CHECK(changes.front().queryWindows.front().total == std::optional<std::size_t>{2});
    REQUIRE(changes.front().searchWindows.size() == 1);
    CHECK(changes.front().searchWindows.front().queryKey == QStringLiteral("affected-search"));
    CHECK(changes.front().searchWindows.front().offset == 25);
    CHECK(changes.front().searchWindows.front().limit == 25);
    CHECK(changes.front().searchWindows.front().total == std::optional<std::size_t>{50});
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

    const auto encoded = actionPayload<javelin::protocol::actions::ContactRequestRefresh>(
        std::string{"missing-owner"});
    const javelin::protocol::CommandRequest request{
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::ContactRequestRefresh::id,
                .payload = encoded,
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
        services,
        eventSink,
        [epoch] { return epoch; },
        []() -> std::optional<javelin::protocol::BoundaryError> { return std::nullopt; },
        [](javelin::app::AccountAuthenticationResult result) { return result; },
        [](javelin::app::AccountConnectionSettings settings)
            -> std::variant<javelin::app::AccountConnectionSettings, QString> { return settings; },
        [](javelin::app::OAuthRevocationRequest request)
            -> std::variant<javelin::app::OAuthRevocationRequest, QString> { return request; }};

    const javelin::protocol::CommandId commandId{.value = QUuid::createUuid()};
    const auto first = dispatcher.dispatch({
        .id = commandId,
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::WorkSummary::id,
                .payload = actionPayload<javelin::protocol::actions::WorkSummary>(),
            },
    });
    const auto* summaryAccepted = std::get_if<javelin::protocol::CommandAccepted>(&first);
    REQUIRE(summaryAccepted != nullptr);
    CHECK(summaryAccepted->epoch == epoch);
    CHECK(summaryAccepted->changedDomains.empty());

    const auto jobId =
        actionPayload<javelin::protocol::actions::WorkRetry>(std::string{"missing-job"});
    const auto workMutation = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::WorkRetry::id,
                .payload = jobId,
            },
    });
    const auto* workAccepted = std::get_if<javelin::protocol::CommandAccepted>(&workMutation);
    REQUIRE(workAccepted != nullptr);
    CHECK(workAccepted->epoch == epoch);
    CHECK(workAccepted->changedDomains ==
          std::vector{javelin::protocol::ChangedDomain::BackgroundJobs});

    const auto differentJobId =
        actionPayload<javelin::protocol::actions::WorkRetry>(std::string{"different-job"});
    const auto changedPayloadReplay = dispatcher.dispatch({
        .id = workAccepted->id,
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::WorkRetry::id,
                .payload = differentJobId,
            },
    });
    const auto* changedPayloadRejected =
        std::get_if<javelin::protocol::CommandRejected>(&changedPayloadReplay);
    REQUIRE(changedPayloadRejected != nullptr);
    CHECK(changedPayloadRejected->error.code ==
          javelin::protocol::BoundaryErrorCode::InvalidRequest);

    dispatcher.releaseGuiResources();
    const auto reused = dispatcher.dispatch({
        .id = commandId,
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::WorkList::id,
                .payload = actionPayload<javelin::protocol::actions::WorkList>(),
            },
    });
    const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reused);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == javelin::protocol::BoundaryErrorCode::InvalidRequest);

    const auto acknowledgementPayload =
        actionPayload<javelin::protocol::actions::AcknowledgeRemoteActionResult>(
            commandId.value.toString(QUuid::WithoutBraces));
    const auto acknowledgement = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::AcknowledgeRemoteActionResult::id,
                .payload = acknowledgementPayload,
            },
    });
    REQUIRE(std::holds_alternative<javelin::protocol::CommandAccepted>(acknowledgement));

    const auto reusedAfterAcknowledgement = dispatcher.dispatch({
        .id = commandId,
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::WorkList::id,
                .payload = actionPayload<javelin::protocol::actions::WorkList>(),
            },
    });
    CHECK(std::holds_alternative<javelin::protocol::CommandAccepted>(reusedAfterAcknowledgement));
}

TEST_CASE("daemon action boundary rejects unknown actions and oversized payloads",
          "[app][daemon][ipc][protocol]")
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

    struct EventSink final : javelin::protocol::BoundaryEventSink
    {
        void onBoundaryEvent(const javelin::protocol::BoundaryEvent&) override
        {
        }
    } eventSink;

    javelin::app::DaemonRemoteActionDispatcher dispatcher{
        services,
        eventSink,
        [] { return javelin::protocol::InvalidationEpoch{.value = 1}; },
        []() -> std::optional<javelin::protocol::BoundaryError> { return std::nullopt; },
        [](javelin::app::AccountAuthenticationResult result) { return result; },
        [](javelin::app::AccountConnectionSettings settings)
            -> std::variant<javelin::app::AccountConnectionSettings, QString> { return settings; },
        [](javelin::app::OAuthRevocationRequest request)
            -> std::variant<javelin::app::OAuthRevocationRequest, QString> { return request; }};

    const auto unknown = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = {.value = 65000},
                .payload = {},
            },
    });
    const auto* unknownRejected = std::get_if<javelin::protocol::CommandRejected>(&unknown);
    REQUIRE(unknownRejected != nullptr);
    CHECK(unknownRejected->error.code ==
          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);

    const auto oversized = dispatcher.dispatch({
        .id = {.value = QUuid::createUuid()},
        .command =
            javelin::protocol::RemoteActionCommand{
                .action = javelin::protocol::actions::WorkSummary::id,
                .payload = QByteArray(
                    static_cast<qsizetype>(
                        javelin::protocol::actions::WorkSummary::maximumPayloadBytes + 1),
                    'x'),
            },
    });
    const auto* oversizedRejected = std::get_if<javelin::protocol::CommandRejected>(&oversized);
    REQUIRE(oversizedRejected != nullptr);
    CHECK(oversizedRejected->error.code == javelin::protocol::BoundaryErrorCode::ValueTooLarge);
}

TEST_CASE("daemon configures only cached JMAP accounts with the Mail capability",
          "[app][daemon][settings][capabilities]")
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
        .connectionName = QStringLiteral("daemon-mail-capability-test"),
        .databasePath = process.databasePath(),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    QSqlQuery seed{connection.database()};
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,name,cap_mail) "
        "VALUES('mail-account','user@example.test','https://example.test/jmap',1,'Mail',1)")));
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary,name,cap_mail) "
        "VALUES('principal-account','','',0,'',0)")));
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name,role,is_subscribed) "
                       "VALUES('mail-account','inbox','Inbox','inbox',1)")));
    REQUIRE(seed.exec(
        QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name,role,is_subscribed) "
                       "VALUES('principal-account','unexpected','Unexpected','',1)")));

    const auto treeResult = javelin::app::loadMailboxTreeCache(process.databasePath());
    const auto* tree = std::get_if<javelin::app::MailboxTreeCacheSnapshot>(&treeResult);
    REQUIRE(tree != nullptr);
    REQUIRE(tree->accounts.size() == 1);
    CHECK(tree->accounts.front().accountId == "mail-account");
    CHECK(tree->mailboxesByAccount.contains("mail-account"));
    CHECK_FALSE(tree->mailboxesByAccount.contains("principal-account"));

    const auto connectionId = QStringLiteral("mail-capability-connection");
    REQUIRE_FALSE(testCredentialStore()
                      ->store(connectionId, {.accessToken = QStringLiteral("secret"),
                                             .refreshToken = {},
                                             .registrationAccessToken = {}})
                      .has_value());
    const auto currentSettings = process.handleGetSettings({});
    const auto* current = std::get_if<javelin::protocol::SettingsSnapshotReply>(&currentSettings);
    REQUIRE(current != nullptr);
    const auto update = process.handleUpdateSettings({
        .baseRevision = current->snapshot.revision,
        .update = {.accounts = std::vector{javelin::protocol::AccountSettings{
                       .id = connectionId,
                       .revision = 0,
                       .displayName = QStringLiteral("Example"),
                       .sessionUrl = QStringLiteral("https://example.test/jmap"),
                       .loginEmail = QStringLiteral("user@example.test"),
                       .tokenEndpoint = {},
                       .oauthClientId = {},
                       .hasCredentials = true,
                       .credentialHandle = {},
                       .tokenExpiresAtEpochSeconds = 0,
                       .cachedAccountIds = {QStringLiteral("mail-account"),
                                            QStringLiteral("principal-account")},
                   }},
                   .syncedMailboxSelections =
                       std::vector{javelin::protocol::MailboxSelectionSettings{
                                       .accountId = QStringLiteral("mail-account"),
                                       .mailboxIds = {QStringLiteral("inbox")},
                                   },
                                   javelin::protocol::MailboxSelectionSettings{
                                       .accountId = QStringLiteral("principal-account"),
                                       .mailboxIds = {QStringLiteral("unexpected")},
                                   }},
                   .notificationMailboxSelections =
                       std::vector<javelin::protocol::MailboxSelectionSettings>{},
                   .remoteContentSenders = std::nullopt,
                   .remoteContentDomains = std::nullopt,
                   .appearance = std::nullopt,
                   .attachments = std::nullopt,
                   .undoSendDelaySeconds = std::nullopt,
                   .workspace = std::nullopt},
    });
    REQUIRE(std::holds_alternative<javelin::protocol::SettingsUpdated>(update));

    QSqlQuery configuredScopes{connection.database()};
    REQUIRE(configuredScopes.exec(QStringLiteral(
        "SELECT account_id FROM offline_mailbox_scopes WHERE desired=1 ORDER BY account_id")));
    REQUIRE(configuredScopes.next());
    CHECK(configuredScopes.value(0).toString() == QStringLiteral("mail-account"));
    CHECK_FALSE(configuredScopes.next());

    process.stop();
    REQUIRE_FALSE(testCredentialStore()->remove(connectionId).has_value());
}
