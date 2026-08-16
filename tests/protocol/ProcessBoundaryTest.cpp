#include "protocol/InProcessEndpoint.h"
#include "protocol/LocalActivationClient.h"
#include "protocol/LocalActivationServer.h"
#include "protocol/LocalDaemonClient.h"
#include "protocol/LocalDaemonServer.h"
#include "protocol/SocketFrameCodec.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLocalSocket>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <mutex>
#include <utility>

namespace
{

    using namespace javelin::protocol;

    int testArgc = 1;
    char testProgramName[] = "javelin_protocol_tests";
    char* testArgv[] = {testProgramName, nullptr};
    QCoreApplication testApplication{testArgc, testArgv};

    class RecordingHandler final : public DaemonRequestHandler
    {
      public:
        HandshakeReply handleHello(const HelloRequest&) override
        {
            return ReadyReply{.protocol = {.major = 1, .minor = 0},
                              .daemon = {.value = QUuid::createUuid()},
                              .cache = {.instance = {.value = QUuid::createUuid()},
                                        .schema = {.value = 3},
                                        .dataVersion = {.value = 7}},
                              .cacheDatabasePath = QStringLiteral("/tmp/javelin/cache.sqlite3"),
                              .epoch = {.value = 12},
                              .settingsRevision = {.value = 5}};
        }

        CommandReply handleCommand(CommandRequest request) override
        {
            receivedCommand = std::move(request);
            if (onCommand)
                onCommand();
            if (returnInvalidBoundaryError)
            {
                return CommandRejected{.id = receivedCommand->id,
                                       .error = {.code = static_cast<BoundaryErrorCode>(255),
                                                 .field = QStringLiteral("command"),
                                                 .detail = QStringLiteral("invalid test enum")}};
            }
            if (const auto* remote = std::get_if<RemoteActionCommand>(&receivedCommand->command))
            {
                return CommandAccepted{.id = receivedCommand->id,
                                       .operation = std::nullopt,
                                       .epoch = {.value = 12},
                                       .changedDomains = {},
                                       .affectedKeys = {},
                                       .immediateResult = remote->payload};
            }
            return CommandAccepted{.id = receivedCommand->id,
                                   .operation = std::nullopt,
                                   .epoch = {.value = 12},
                                   .changedDomains = {ChangedDomain::MailQueryWindows},
                                   .affectedKeys = {QStringLiteral("account-1")},
                                   .immediateResult = std::nullopt};
        }

        MaterializationReply handleMaterialization(MaterializationRequest request) override
        {
            receivedMaterialization = std::move(request);
            return MaterializationAccepted{.id = receivedMaterialization->id};
        }

        void
        handleCancelMaterializationScope(const CancelMaterializationScopeRequest& request) override
        {
            cancelledScope = request.scope;
        }

        SettingsReadReply handleGetSettings(const GetSettingsRequest&) override
        {
            return SettingsSnapshotReply{
                .snapshot = {
                    .revision = {.value = 5},
                    .schemaVersion = 3,
                    .accounts = {},
                    .syncedMailboxSelections = {},
                    .notificationMailboxSelections = {},
                    .remoteContentSenders = {},
                    .remoteContentDomains = {},
                    .appearance = {},
                    .attachments = {},
                    .undoSendDelaySeconds = 10,
                    .undoSendUsesDialog = false,
                    .workspace = {
                        .formatVersion = 1,
                        .mainWindowState = QByteArrayLiteral("window-state"),
                        .composeRichTextDefault = false,
                        .defaultCalendarDestination = {.ownerAccountId = QStringLiteral("server-1"),
                                                       .accountId = QStringLiteral("account-1"),
                                                       .calendarId =
                                                           QStringLiteral("calendar-default")},
                        .calendarColorOverrides = {{.calendarId = QStringLiteral("calendar-1"),
                                                    .color = QStringLiteral("#123456")}},
                        .emailContextMenuLayout = {QStringLiteral("compose_reply"),
                                                   QStringLiteral("separator"),
                                                   QStringLiteral("archive_email")},
                        .calendarEventContextMenuLayout = {
                            QStringLiteral("calendar_event_edit"), QStringLiteral("separator"),
                            QStringLiteral("calendar_event_delete")}}}};
        }

        SettingsUpdateReply handleUpdateSettings(UpdateSettingsRequest request) override
        {
            receivedSettingsUpdate = std::move(request);
            return SettingsUpdated{.revision = {.value = 6}};
        }

        std::optional<BoundaryError> handleCacheAccessSuspended(
            const CacheAccessSuspendedAcknowledgement& acknowledgement) override
        {
            acknowledgedCache = acknowledgement.instance;
            return std::nullopt;
        }

        std::optional<BoundaryError> handlePing(const PingRequest&) override
        {
            pinged = true;
            return std::nullopt;
        }

        std::optional<BoundaryError> handleGuiReadyForActivation() override
        {
            guiReady = true;
            return std::nullopt;
        }

        std::optional<BoundaryError> handleGuiActivation(const ActivationRoute& route) override
        {
            const std::scoped_lock lock{activationMutex};
            activatedRoute = route;
            return activationError;
        }

        [[nodiscard]] std::optional<ActivationRoute> activatedRouteSnapshot() const
        {
            const std::scoped_lock lock{activationMutex};
            return activatedRoute;
        }

        void setActivationError(std::optional<BoundaryError> error)
        {
            const std::scoped_lock lock{activationMutex};
            activationError = std::move(error);
        }

        std::optional<CommandRequest> receivedCommand;
        std::optional<MaterializationRequest> receivedMaterialization;
        std::optional<UpdateSettingsRequest> receivedSettingsUpdate;
        std::optional<ScopeId> cancelledScope;
        std::optional<CacheInstanceId> acknowledgedCache;
        std::optional<ActivationRoute> activatedRoute;
        std::optional<BoundaryError> activationError;
        mutable std::mutex activationMutex;
        bool pinged = false;
        bool guiReady = false;
        bool returnInvalidBoundaryError = false;
        std::function<void()> onCommand;
    };

    class RecordingSink final : public BoundaryEventSink
    {
      public:
        void onBoundaryEvent(const BoundaryEvent& event) override
        {
            received = event;
        }

        std::optional<BoundaryEvent> received;
    };

    class ReentrantRequestSink final : public BoundaryEventSink
    {
      public:
        explicit ReentrantRequestSink(SocketDaemonClient& client) : m_client(client)
        {
        }

        void onBoundaryEvent(const BoundaryEvent& event) override
        {
            received = event;
            settings = m_client.getSettings();
        }

        std::optional<BoundaryEvent> received;
        std::optional<SettingsReadReply> settings;

      private:
        SocketDaemonClient& m_client;
    };

    [[nodiscard]] CommandRequest refreshRequest()
    {
        return {.id = {.value = QUuid::createUuid()},
                .command =
                    RefreshAccountCommand{.accountId = QStringLiteral("account-1"), .force = true}};
    }

    void exerciseCommonSurface(CommandClient& commandClient,
                               MaterializationClient& materializationClient,
                               SettingsClient& settingsClient,
                               DaemonStatusClient& daemonStatusClient,
                               ActivationClient& activationClient,
                               CacheAccessClient& cacheAccessClient, RecordingHandler& handler)
    {
        const auto handshake =
            daemonStatusClient.hello({.protocol = {.major = 1, .minor = 0},
                                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                                .revision = QStringLiteral("test")}});
        REQUIRE(std::get_if<ReadyReply>(&handshake) != nullptr);

        const auto command = refreshRequest();
        const auto commandReply = commandClient.submitCommand(command);
        const auto* accepted = std::get_if<CommandAccepted>(&commandReply);
        REQUIRE(accepted != nullptr);
        if (accepted == nullptr)
            return;
        CHECK(accepted->id == command.id);
        REQUIRE(handler.receivedCommand.has_value());

        const CommandRequest remoteCommand{.id = {.value = QUuid::createUuid()},
                                           .command = RemoteActionCommand{
                                               .action = ActionId{.value = 66},
                                               .payload = QByteArray{"remote\0payload", 14},
                                           }};
        const auto remoteReply = commandClient.submitCommand(remoteCommand);
        const auto* remoteAccepted = std::get_if<CommandAccepted>(&remoteReply);
        REQUIRE(remoteAccepted != nullptr);
        REQUIRE(remoteAccepted->immediateResult.has_value());
        CHECK(*remoteAccepted->immediateResult == QByteArray{"remote\0payload", 14});
        REQUIRE(handler.receivedCommand.has_value());
        CHECK(handler.receivedCommand->command == remoteCommand.command);

        const MaterializationRequest materialization{
            .id = {.value = QUuid::createUuid()},
            .scope = {.value = QUuid::createUuid()},
            .request = MailboxWindowMaterialization{.accountId = QStringLiteral("account-1"),
                                                    .mailboxId = QStringLiteral("inbox"),
                                                    .offset = 20,
                                                    .limit = 25}};
        const auto materializationReply =
            materializationClient.requestMaterialization(materialization);
        REQUIRE(std::holds_alternative<MaterializationAccepted>(materializationReply));
        CHECK(std::get<MaterializationAccepted>(materializationReply).id == materialization.id);
        materializationClient.cancelMaterializationScope(materialization.scope);
        REQUIRE(handler.cancelledScope.has_value());
        CHECK(*handler.cancelledScope == materialization.scope);

        const auto settings = settingsClient.getSettings();
        REQUIRE(std::holds_alternative<SettingsSnapshotReply>(settings));
        const auto& settingsSnapshot = std::get<SettingsSnapshotReply>(settings).snapshot;
        CHECK(settingsSnapshot.revision.value == 5);
        CHECK(settingsSnapshot.schemaVersion == 3);
        CHECK(settingsSnapshot.workspace.mainWindowState == QByteArrayLiteral("window-state"));
        CHECK_FALSE(settingsSnapshot.workspace.composeRichTextDefault);
        CHECK(settingsSnapshot.workspace.defaultCalendarDestination.ownerAccountId ==
              QStringLiteral("server-1"));
        CHECK(settingsSnapshot.workspace.defaultCalendarDestination.accountId ==
              QStringLiteral("account-1"));
        CHECK(settingsSnapshot.workspace.defaultCalendarDestination.calendarId ==
              QStringLiteral("calendar-default"));
        REQUIRE(settingsSnapshot.workspace.calendarColorOverrides.size() == 1);
        CHECK(settingsSnapshot.workspace.calendarColorOverrides.front().calendarId ==
              QStringLiteral("calendar-1"));

        const auto settingsUpdate = settingsClient.updateSettings(
            {.baseRevision = {.value = 5},
             .update = {
                 .accounts = std::vector<AccountSettings>{{
                     .id = QStringLiteral("connection-1"),
                     .revision = 4,
                     .displayName = QStringLiteral("Alice"),
                     .sessionUrl = QStringLiteral("https://mail.example.com/.well-known/jmap"),
                     .loginEmail = QStringLiteral("alice@example.com"),
                     .tokenEndpoint = QStringLiteral("https://mail.example.com/token"),
                     .oauthClientId = QStringLiteral("client-id"),
                     .oauthIssuer = QStringLiteral("https://auth.example.com"),
                     .oauthResource = QStringLiteral("https://mail.example.com/jmap"),
                     .oauthScope = QStringLiteral("mail offline_access"),
                     .revocationEndpoint = QStringLiteral("https://auth.example.com/revoke"),
                     .registrationClientUri =
                         QStringLiteral("https://auth.example.com/register/client-id"),
                     .hasCredentials = true,
                     .credentialHandle = QStringLiteral("one-time-handle"),
                     .tokenExpiresAtEpochSeconds = 1'785'784'100,
                     .reauthenticationRequired = true,
                     .cachedAccountIds = {QStringLiteral("account-1")},
                 }},
                 .syncedMailboxSelections = std::nullopt,
                 .notificationMailboxSelections = std::nullopt,
                 .remoteContentSenders = std::nullopt,
                 .remoteContentDomains = std::nullopt,
                 .appearance = std::nullopt,
                 .attachments = std::nullopt,
                 .undoSendDelaySeconds = std::nullopt,
                 .undoSendUsesDialog = true,
                 .workspace = WorkspaceSettings{
                     .formatVersion = 1,
                     .mainWindowState = QByteArrayLiteral("updated-window-state"),
                     .composeRichTextDefault = true,
                     .defaultCalendarDestination = {.ownerAccountId = QStringLiteral("server-2"),
                                                    .accountId = QStringLiteral("account-2"),
                                                    .calendarId = QStringLiteral("calendar-2")},
                     .calendarColorOverrides = {{.calendarId = QStringLiteral("calendar-2"),
                                                 .color = QStringLiteral("#abcdef")}},
                     .emailContextMenuLayout = {QStringLiteral("archive_email")},
                     .calendarEventContextMenuLayout = {
                         QStringLiteral("calendar_event_copy_details")}}}});
        REQUIRE(std::holds_alternative<SettingsUpdated>(settingsUpdate));
        CHECK(std::get<SettingsUpdated>(settingsUpdate).revision.value == 6);
        REQUIRE(handler.receivedSettingsUpdate.has_value());
        REQUIRE(handler.receivedSettingsUpdate->update.undoSendUsesDialog == true);
        REQUIRE(handler.receivedSettingsUpdate->update.accounts.has_value());
        REQUIRE(handler.receivedSettingsUpdate->update.accounts->size() == 1);
        const auto& account = handler.receivedSettingsUpdate->update.accounts->front();
        CHECK(account.tokenEndpoint == QStringLiteral("https://mail.example.com/token"));
        REQUIRE(handler.receivedSettingsUpdate->update.workspace.has_value());
        CHECK(handler.receivedSettingsUpdate->update.workspace->emailContextMenuLayout ==
              std::vector<QString>{QStringLiteral("archive_email")});
        CHECK(handler.receivedSettingsUpdate->update.workspace->calendarEventContextMenuLayout ==
              std::vector<QString>{QStringLiteral("calendar_event_copy_details")});
        CHECK(account.oauthClientId == QStringLiteral("client-id"));
        CHECK(account.oauthIssuer == QStringLiteral("https://auth.example.com"));
        CHECK(account.oauthResource == QStringLiteral("https://mail.example.com/jmap"));
        CHECK(account.oauthScope == QStringLiteral("mail offline_access"));
        CHECK(account.revocationEndpoint == QStringLiteral("https://auth.example.com/revoke"));
        CHECK(account.registrationClientUri ==
              QStringLiteral("https://auth.example.com/register/client-id"));
        CHECK(account.hasCredentials);
        CHECK(account.credentialHandle == QStringLiteral("one-time-handle"));
        CHECK(account.tokenExpiresAtEpochSeconds == 1'785'784'100);
        CHECK(account.reauthenticationRequired);
        REQUIRE(handler.receivedSettingsUpdate->update.workspace.has_value());
        CHECK(handler.receivedSettingsUpdate->update.workspace->mainWindowState ==
              QByteArrayLiteral("updated-window-state"));
        CHECK(handler.receivedSettingsUpdate->update.workspace->composeRichTextDefault);
        CHECK(handler.receivedSettingsUpdate->update.workspace->defaultCalendarDestination
                  .ownerAccountId == QStringLiteral("server-2"));
        CHECK(handler.receivedSettingsUpdate->update.workspace->defaultCalendarDestination
                  .accountId == QStringLiteral("account-2"));
        CHECK(handler.receivedSettingsUpdate->update.workspace->defaultCalendarDestination
                  .calendarId == QStringLiteral("calendar-2"));
        REQUIRE(handler.receivedSettingsUpdate->update.workspace->calendarColorOverrides.size() ==
                1);
        CHECK(handler.receivedSettingsUpdate->update.workspace->calendarColorOverrides.front()
                  .calendarId == QStringLiteral("calendar-2"));

        CHECK_FALSE(daemonStatusClient.ping().has_value());
        CHECK_FALSE(activationClient.readyForActivation().has_value());
        CHECK_FALSE(
            cacheAccessClient
                .acknowledgeCacheAccessSuspended({.instance = {.value = QUuid::createUuid()}})
                .has_value());
    }

    [[nodiscard]] SocketEndpointOptions socketOptions(const QTemporaryDir& directory)
    {
        return {.runtimeDirectory = directory.path(),
                .socketPath = QDir{directory.path()}.filePath(QStringLiteral("javelind.sock")),
                .limits = {},
                .protocol = {.major = 1, .minor = 0},
                .expectedBuild = BuildIdentity{.application = QStringLiteral("Javelin-Mail"),
                                               .revision = QStringLiteral("test")},
                .maximumQueuedFrames = 8,
                .maximumQueuedBytes = 4096,
                .responseTimeoutMilliseconds = 2000,
                .enforcePeerCredentials = true};
    }

    template <typename Predicate> void processUntil(Predicate predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(1);
        }
    }

    class SocketEndpointThread final
    {
      public:
        SocketEndpointThread(RecordingHandler& handler, SocketEndpointOptions options)
            : m_endpoint(new SocketDaemonEndpoint(handler, std::move(options)))
        {
            m_endpoint->moveToThread(&m_thread);
            m_thread.start();
        }

        SocketEndpointThread(const SocketEndpointThread&) = delete;
        SocketEndpointThread& operator=(const SocketEndpointThread&) = delete;

        ~SocketEndpointThread()
        {
            if (m_endpoint != nullptr)
            {
                auto* endpoint = m_endpoint;
                QMetaObject::invokeMethod(
                    endpoint,
                    [this, endpoint]
                    {
                        endpoint->close();
                        delete endpoint;
                        m_endpoint = nullptr;
                    },
                    Qt::BlockingQueuedConnection);
            }
            m_thread.quit();
            m_thread.wait();
        }

        [[nodiscard]] std::optional<SocketTransportError> listen()
        {
            std::optional<SocketTransportError> error;
            QMetaObject::invokeMethod(
                m_endpoint, [this, &error] { error = m_endpoint->listen(); },
                Qt::BlockingQueuedConnection);
            return error;
        }

        void close()
        {
            QMetaObject::invokeMethod(
                m_endpoint, [this] { m_endpoint->close(); }, Qt::BlockingQueuedConnection);
        }

        void publishEvent(const BoundaryEvent& event)
        {
            QMetaObject::invokeMethod(
                m_endpoint, [this, event] { m_endpoint->publishEvent(event); },
                Qt::BlockingQueuedConnection);
        }

        [[nodiscard]] std::optional<SocketTransportError> lastError() const
        {
            std::optional<SocketTransportError> error;
            QMetaObject::invokeMethod(
                m_endpoint, [this, &error] { error = m_endpoint->lastError(); },
                Qt::BlockingQueuedConnection);
            return error;
        }

        [[nodiscard]] SocketDaemonEndpoint* endpointForThreadCallback() const
        {
            return m_endpoint;
        }

      private:
        QThread m_thread;
        SocketDaemonEndpoint* m_endpoint = nullptr;
    };

    class SocketActivationEndpointThread final
    {
      public:
        SocketActivationEndpointThread(RecordingHandler& handler, SocketEndpointOptions options)
            : m_endpoint(new SocketActivationEndpoint(handler, std::move(options)))
        {
            m_endpoint->moveToThread(&m_thread);
            m_thread.start();
        }

        SocketActivationEndpointThread(const SocketActivationEndpointThread&) = delete;
        SocketActivationEndpointThread& operator=(const SocketActivationEndpointThread&) = delete;

        ~SocketActivationEndpointThread()
        {
            if (m_endpoint != nullptr)
            {
                auto* endpoint = m_endpoint;
                QMetaObject::invokeMethod(
                    endpoint,
                    [this, endpoint]
                    {
                        endpoint->close();
                        delete endpoint;
                        m_endpoint = nullptr;
                    },
                    Qt::BlockingQueuedConnection);
            }
            m_thread.quit();
            m_thread.wait();
        }

        [[nodiscard]] std::optional<SocketTransportError> listen()
        {
            std::optional<SocketTransportError> error;
            QMetaObject::invokeMethod(
                m_endpoint, [this, &error] { error = m_endpoint->listen(); },
                Qt::BlockingQueuedConnection);
            return error;
        }

        void close()
        {
            QMetaObject::invokeMethod(
                m_endpoint, [this] { m_endpoint->close(); }, Qt::BlockingQueuedConnection);
        }

      private:
        QThread m_thread;
        SocketActivationEndpoint* m_endpoint = nullptr;
    };

} // namespace

TEST_CASE("in-process endpoint carries typed command admission", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    const auto request = refreshRequest();
    const auto reply = endpoint.submitCommand(request);

    const auto* accepted = std::get_if<CommandAccepted>(&reply);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->id == request.id);
    CHECK(accepted->epoch.value == 12);
    REQUIRE(handler.receivedCommand.has_value());
    const auto* refresh = std::get_if<RefreshAccountCommand>(&handler.receivedCommand->command);
    REQUIRE(refresh != nullptr);
    CHECK(refresh->accountId == QStringLiteral("account-1"));
    CHECK(refresh->force);
}

TEST_CASE("in-process endpoint runs the transport-neutral typed surface", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    exerciseCommonSurface(endpoint, endpoint, endpoint, endpoint, endpoint, endpoint, handler);
}

TEST_CASE("invalid typed commands are rejected before daemon dispatch", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    const CommandRequest request{.id = {}, .command = RefreshAccountCommand{}};
    const auto reply = endpoint.submitCommand(request);

    const auto* rejected = std::get_if<CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->id == request.id);
    CHECK(rejected->error.code == BoundaryErrorCode::InvalidIdentifier);
    CHECK_FALSE(handler.receivedCommand.has_value());
}

TEST_CASE("materialization requests retain request and scope identity", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    const MaterializationRequest request{
        .id = {.value = QUuid::createUuid()},
        .scope = {.value = QUuid::createUuid()},
        .request = MailboxWindowMaterialization{.accountId = QStringLiteral("account-1"),
                                                .mailboxId = QStringLiteral("inbox"),
                                                .offset = 40,
                                                .limit = 50}};
    const auto reply = endpoint.requestMaterialization(request);

    const auto* accepted = std::get_if<MaterializationAccepted>(&reply);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->id == request.id);
    REQUIRE(handler.receivedMaterialization.has_value());
    CHECK(handler.receivedMaterialization->scope == request.scope);

    endpoint.cancelMaterializationScope(request.scope);
    REQUIRE(handler.cancelledScope.has_value());
    CHECK(*handler.cancelledScope == request.scope);
}

TEST_CASE("endpoint validates bounded values and estimates their frame size", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler, {.maximumStringBytes = 8}};

    const auto request = refreshRequest();
    const auto reply = endpoint.submitCommand(request);
    const auto* rejected = std::get_if<CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::ValueTooLarge);
    CHECK(estimatedEncodedSize(ClientRequest{request}) < BoundaryLimits{}.maximumFrameBytes);
}

TEST_CASE("settings collections enforce protocol bounds", "[protocol][settings]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler, {.maximumCollectionItems = 2}};
    SettingsUpdate update;
    update.remoteContentSenders =
        std::vector{QStringLiteral("one@example.test"), QStringLiteral("two@example.test"),
                    QStringLiteral("three@example.test")};

    const auto reply =
        endpoint.updateSettings({.baseRevision = {.value = 5}, .update = std::move(update)});
    const auto* rejected = std::get_if<SettingsUpdateRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::TooManyValues);
    CHECK(rejected->error.field == QStringLiteral("update.remoteContentSenders"));
    CHECK_FALSE(handler.receivedSettingsUpdate.has_value());
}

TEST_CASE("workspace settings enforce protocol bounds", "[protocol][settings]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler, {.maximumWorkspaceBytes = 4}};
    SettingsUpdate update;
    update.workspace = WorkspaceSettings{
        .formatVersion = 1,
        .mainWindowState = QByteArrayLiteral("large"),
        .composeRichTextDefault = true,
        .defaultCalendarDestination = {},
        .calendarColorOverrides = {},
        .emailContextMenuLayout = {QStringLiteral("compose_reply")},
        .calendarEventContextMenuLayout = {},
    };

    const auto reply =
        endpoint.updateSettings({.baseRevision = {.value = 5}, .update = std::move(update)});
    const auto* rejected = std::get_if<SettingsUpdateRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::ValueTooLarge);
    CHECK(rejected->error.field == QStringLiteral("update.workspace.mainWindowState"));
    CHECK_FALSE(handler.receivedSettingsUpdate.has_value());
}

TEST_CASE("email context menu settings enforce collection bounds", "[protocol][settings]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler, {.maximumCollectionItems = 1}};
    SettingsUpdate update;
    update.workspace = WorkspaceSettings{
        .formatVersion = 1,
        .mainWindowState = {},
        .composeRichTextDefault = true,
        .defaultCalendarDestination = {},
        .calendarColorOverrides = {},
        .emailContextMenuLayout = {QStringLiteral("compose_reply"),
                                   QStringLiteral("archive_email")},
        .calendarEventContextMenuLayout = {},
    };

    const auto reply =
        endpoint.updateSettings({.baseRevision = {.value = 5}, .update = std::move(update)});
    const auto* rejected = std::get_if<SettingsUpdateRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::TooManyValues);
    CHECK(rejected->error.field == QStringLiteral("update.workspace.emailContextMenuLayout"));
    CHECK_FALSE(handler.receivedSettingsUpdate.has_value());
}

TEST_CASE("calendar event context menu settings enforce collection bounds", "[protocol][settings]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler, {.maximumCollectionItems = 1}};
    SettingsUpdate update;
    update.workspace = WorkspaceSettings{
        .formatVersion = 1,
        .mainWindowState = {},
        .composeRichTextDefault = true,
        .defaultCalendarDestination = {},
        .calendarColorOverrides = {},
        .emailContextMenuLayout = {},
        .calendarEventContextMenuLayout = {QStringLiteral("calendar_event_edit"),
                                           QStringLiteral("calendar_event_delete")},
    };

    const auto reply =
        endpoint.updateSettings({.baseRevision = {.value = 5}, .update = std::move(update)});
    const auto* rejected = std::get_if<SettingsUpdateRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::TooManyValues);
    CHECK(rejected->error.field ==
          QStringLiteral("update.workspace.calendarEventContextMenuLayout"));
    CHECK_FALSE(handler.receivedSettingsUpdate.has_value());
}

TEST_CASE("endpoint exposes settings, handshake, lifecycle and events through typed ports",
          "[protocol]")
{
    RecordingHandler handler;
    RecordingSink sink;
    InProcessEndpoint endpoint{handler};
    REQUIRE_FALSE(endpoint.attachEventSink(sink).has_value());

    const auto handshake = endpoint.hello({.protocol = {.major = 1, .minor = 0},
                                           .build = {.application = QStringLiteral("Javelin-Mail"),
                                                     .revision = QStringLiteral("test")}});
    REQUIRE(std::get_if<ReadyReply>(&handshake) != nullptr);
    const auto settings = endpoint.getSettings();
    const auto* snapshot = std::get_if<SettingsSnapshotReply>(&settings);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot.revision.value == 5);
    CHECK(snapshot->snapshot.workspace.emailContextMenuLayout ==
          std::vector<QString>{QStringLiteral("compose_reply"), QStringLiteral("separator"),
                               QStringLiteral("archive_email")});
    CHECK(snapshot->snapshot.workspace.calendarEventContextMenuLayout ==
          std::vector<QString>{QStringLiteral("calendar_event_edit"), QStringLiteral("separator"),
                               QStringLiteral("calendar_event_delete")});

    CHECK_FALSE(endpoint.ping().has_value());
    CHECK(handler.pinged);
    CHECK_FALSE(endpoint.readyForActivation().has_value());
    CHECK(handler.guiReady);

    endpoint.publishEvent(CacheInvalidation{.epoch = {.value = 13},
                                            .changedDomains = {ChangedDomain::MessageMetadata},
                                            .affectedKeys = {QStringLiteral("email-1")}});
    REQUIRE(sink.received.has_value());
    const auto* invalidation = std::get_if<CacheInvalidation>(&*sink.received);
    REQUIRE(invalidation != nullptr);
    CHECK(invalidation->epoch.value == 13);

    endpoint.publishEvent(
        OperationFailed{.operation = {.value = QUuid::createUuid()},
                        .error = {.code = BoundaryErrorCode::Busy,
                                  .field = QStringLiteral("refresh"),
                                  .detail = QStringLiteral("operation was deferred")}});
    REQUIRE(sink.received.has_value());
    const auto* failure = std::get_if<OperationFailed>(&*sink.received);
    REQUIRE(failure != nullptr);
    CHECK(failure->error.code == BoundaryErrorCode::Busy);

    const OperationCompleted completion{
        .operation = {.value = QUuid::createUuid()},
        .result = QByteArray{"result\0bytes", 12},
    };
    endpoint.publishEvent(completion);
    REQUIRE(sink.received.has_value());
    const auto* completed = std::get_if<OperationCompleted>(&*sink.received);
    REQUIRE(completed != nullptr);
    CHECK(completed->operation == completion.operation);
    CHECK(completed->result == completion.result);

    endpoint.detachEventSink(sink);
    const auto update =
        endpoint.updateSettings({.baseRevision = {.value = 5},
                                 .update = {.accounts = std::nullopt,
                                            .syncedMailboxSelections = std::nullopt,
                                            .notificationMailboxSelections = std::nullopt,
                                            .remoteContentSenders = std::nullopt,
                                            .remoteContentDomains = std::nullopt,
                                            .appearance = std::nullopt,
                                            .attachments = std::nullopt,
                                            .undoSendDelaySeconds = std::nullopt,
                                            .undoSendUsesDialog = std::nullopt,
                                            .workspace = std::nullopt}});
    CHECK(std::holds_alternative<SettingsUpdated>(update));
    REQUIRE(handler.receivedSettingsUpdate.has_value());
    CHECK_FALSE(handler.receivedSettingsUpdate->update.appearance.has_value());
}

TEST_CASE("socket frame codec handles partial, oversized, and unknown frames", "[protocol][socket]")
{
    const auto encoded = encodeSocketFrame(SocketFrameKind::PingRequest, 42, {});
    REQUIRE(std::holds_alternative<QByteArray>(encoded));
    const auto bytes = std::get<QByteArray>(encoded);

    SocketFrameDecoder decoder;
    REQUIRE_FALSE(decoder.append(bytes.left(5)).has_value());
    auto partial = decoder.takeFrame();
    REQUIRE(std::holds_alternative<std::optional<SocketFrame>>(partial));
    CHECK_FALSE(std::get<std::optional<SocketFrame>>(partial).has_value());
    REQUIRE_FALSE(decoder.append(bytes.mid(5)).has_value());
    auto complete = decoder.takeFrame();
    REQUIRE(std::holds_alternative<std::optional<SocketFrame>>(complete));
    REQUIRE(std::get<std::optional<SocketFrame>>(complete).has_value());
    CHECK(std::get<std::optional<SocketFrame>>(complete)->kind == SocketFrameKind::PingRequest);
    CHECK(std::get<std::optional<SocketFrame>>(complete)->correlation == 42);

    QByteArray unknown = bytes;
    unknown[6] = static_cast<char>(0x7f);
    unknown[7] = static_cast<char>(0xff);
    SocketFrameDecoder unknownDecoder;
    REQUIRE_FALSE(unknownDecoder.append(unknown).has_value());
    const auto unknownResult = unknownDecoder.takeFrame();
    REQUIRE(std::holds_alternative<SocketFrameError>(unknownResult));
    CHECK(std::get<SocketFrameError>(unknownResult).code ==
          SocketFrameErrorCode::UnknownMessageKind);

    QByteArray unsupportedVersion = bytes;
    unsupportedVersion[4] = static_cast<char>(0x7f);
    unsupportedVersion[5] = static_cast<char>(0xff);
    SocketFrameDecoder unsupportedVersionDecoder;
    REQUIRE_FALSE(unsupportedVersionDecoder.append(unsupportedVersion).has_value());
    const auto unsupportedVersionResult = unsupportedVersionDecoder.takeFrame();
    REQUIRE(std::holds_alternative<SocketFrameError>(unsupportedVersionResult));
    CHECK(std::get<SocketFrameError>(unsupportedVersionResult).code ==
          SocketFrameErrorCode::UnsupportedVersion);

    BoundaryLimits smallLimits{.maximumFrameBytes = 32};
    const auto oversized = encodeSocketFrame(SocketFrameKind::PingRequest, 1, QByteArray(16, 'x'),
                                             smallLimits.maximumFrameBytes);
    REQUIRE(std::holds_alternative<SocketFrameError>(oversized));
    CHECK(std::get<SocketFrameError>(oversized).code == SocketFrameErrorCode::FrameTooLarge);

    QByteArray invalidVariant;
    invalidVariant.append(static_cast<char>(9));
    const auto malformed = encodeSocketFrame(SocketFrameKind::CommandRequest, 2, invalidVariant);
    REQUIRE(std::holds_alternative<QByteArray>(malformed));
    SocketFrameDecoder malformedDecoder;
    REQUIRE_FALSE(malformedDecoder.append(std::get<QByteArray>(malformed)).has_value());
    const auto malformedFrame = malformedDecoder.takeFrame();
    REQUIRE(std::holds_alternative<std::optional<SocketFrame>>(malformedFrame));
    CHECK(std::get<std::optional<SocketFrame>>(malformedFrame)->kind ==
          SocketFrameKind::CommandRequest);
}

TEST_CASE("socket frame decoder handles every deterministic chunk boundary",
          "[protocol][socket][property]")
{
    for (const qsizetype payloadSize : std::array<qsizetype, 6>{0, 1, 7, 31, 127, 1024})
    {
        QByteArray payload(payloadSize, '\0');
        for (qsizetype index = 0; index < payload.size(); ++index)
            payload[index] = static_cast<char>((index * 37 + payloadSize) & 0xff);

        const auto encoded =
            encodeSocketFrame(SocketFrameKind::BoundaryEventFrame, 0x12345678, payload);
        const auto* bytes = std::get_if<QByteArray>(&encoded);
        REQUIRE(bytes != nullptr);

        for (std::uint32_t seed = 1; seed <= 16; ++seed)
        {
            SocketFrameDecoder decoder;
            qsizetype offset = 0;
            std::uint32_t state = seed;
            while (offset < bytes->size())
            {
                state = state * 1664525U + 1013904223U;
                const qsizetype chunk = std::min<qsizetype>(1 + static_cast<qsizetype>(state % 23U),
                                                            bytes->size() - offset);
                CHECK_FALSE(decoder.append(bytes->mid(offset, chunk)).has_value());
                offset += chunk;
            }

            const auto decoded = decoder.takeFrame();
            const auto* frame = std::get_if<std::optional<SocketFrame>>(&decoded);
            REQUIRE(frame != nullptr);
            REQUIRE(frame->has_value());
            CHECK((*frame)->kind == SocketFrameKind::BoundaryEventFrame);
            CHECK((*frame)->correlation == 0x12345678);
            CHECK((*frame)->payload == payload);
        }

        for (qsizetype length = 0; length < bytes->size(); ++length)
        {
            SocketFrameDecoder decoder;
            CHECK_FALSE(decoder.append(bytes->left(length)).has_value());
            const auto decoded = decoder.takeFrame();
            const auto* frame = std::get_if<std::optional<SocketFrame>>(&decoded);
            REQUIRE(frame != nullptr);
            CHECK_FALSE(frame->has_value());
        }
    }
}

TEST_CASE("socket endpoint runs the transport-neutral typed surface", "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    RecordingHandler handler;
    const auto options = socketOptions(runtimeDirectory);
    SocketEndpointThread endpoint{handler, options};
    const auto listenError = endpoint.listen();
    REQUIRE_FALSE(listenError.has_value());

    SocketDaemonClient client{options};
    RecordingSink sink;
    REQUIRE_FALSE(client.attachEventSink(sink).has_value());
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    exerciseCommonSurface(client, client, client, client, client, client, handler);

    handler.returnInvalidBoundaryError = true;
    const auto invalidReply = client.submitCommand(refreshRequest());
    handler.returnInvalidBoundaryError = false;
    const auto* invalidReplyRejected = std::get_if<CommandRejected>(&invalidReply);
    REQUIRE(invalidReplyRejected != nullptr);
    CHECK(invalidReplyRejected->error.code == BoundaryErrorCode::ProtocolViolation);

    endpoint.publishEvent(CacheInvalidation{
        .epoch = {.value = 13},
        .changedDomains = {ChangedDomain::MessageMetadata},
        .affectedKeys = {QStringLiteral("c")},
        .accountId = QStringLiteral("c"),
        .mailboxIds = {QStringLiteral("c")},
        .messageContentEmailIds = {QStringLiteral("email-1")},
        .mailboxWindows =
            {{.mailboxId = QStringLiteral("c"), .offset = 0, .limit = 100, .total = 113}},
        .searchWindows = {{.queryKey = QStringLiteral("search-1"),
                           .offset = 100,
                           .limit = 50,
                           .total = std::nullopt}},
    });
    processUntil([&sink] { return sink.received.has_value(); });
    REQUIRE(sink.received.has_value());
    const auto* invalidation = std::get_if<CacheInvalidation>(&*sink.received);
    REQUIRE(invalidation != nullptr);
    CHECK(invalidation->epoch.value == 13);
    CHECK(invalidation->accountId == QStringLiteral("c"));
    CHECK(invalidation->mailboxIds == std::vector{QStringLiteral("c")});
    CHECK(invalidation->messageContentEmailIds == std::vector{QStringLiteral("email-1")});
    REQUIRE(invalidation->mailboxWindows.size() == 1);
    CHECK(invalidation->mailboxWindows.front().mailboxId == QStringLiteral("c"));
    CHECK(invalidation->mailboxWindows.front().total == std::optional<std::uint64_t>{113});
    REQUIRE(invalidation->searchWindows.size() == 1);
    CHECK(invalidation->searchWindows.front().queryKey == QStringLiteral("search-1"));

    sink.received.reset();
    endpoint.publishEvent(ThreadMaterializationProgress{
        .accountId = QStringLiteral("account-1"),
        .threadIds = {QStringLiteral("thread-1"), QStringLiteral("thread-2")},
        .inFlight = false,
        .success = false,
        .error = QStringLiteral("temporary failure"),
    });
    processUntil([&sink] { return sink.received.has_value(); });
    REQUIRE(sink.received.has_value());
    const auto* progress = std::get_if<ThreadMaterializationProgress>(&*sink.received);
    REQUIRE(progress != nullptr);
    CHECK(progress->accountId == QStringLiteral("account-1"));
    CHECK(progress->threadIds ==
          std::vector{QStringLiteral("thread-1"), QStringLiteral("thread-2")});
    CHECK_FALSE(progress->inFlight);
    CHECK_FALSE(progress->success);
    CHECK(progress->error == QStringLiteral("temporary failure"));

    sink.received.reset();
    endpoint.publishEvent(DaemonLogEntries{
        .entries = {{.timestampMilliseconds = 123456789,
                     .level = 2,
                     .subsystem = QStringLiteral("daemon.sync"),
                     .message = QStringLiteral("downloaded message")}},
    });
    processUntil([&sink] { return sink.received.has_value(); });
    REQUIRE(sink.received.has_value());
    const auto* logEntries = std::get_if<DaemonLogEntries>(&*sink.received);
    REQUIRE(logEntries != nullptr);
    REQUIRE(logEntries->entries.size() == 1);
    CHECK(logEntries->entries.front().timestampMilliseconds == 123456789);
    CHECK(logEntries->entries.front().level == 2);
    CHECK(logEntries->entries.front().subsystem == QStringLiteral("daemon.sync"));
    CHECK(logEntries->entries.front().message == QStringLiteral("downloaded message"));

    sink.received.reset();
    endpoint.publishEvent(DaemonShutdownRequested{});
    processUntil([&sink] { return sink.received.has_value(); });
    REQUIRE(sink.received.has_value());
    CHECK(std::holds_alternative<DaemonShutdownRequested>(*sink.received));

    for (int index = 0; index < 100; ++index)
    {
        endpoint.publishEvent(
            DaemonStatusChanged{.status = {.lifecycle = DaemonLifecycle::Ready, .accounts = {}}});
    }
    CHECK_FALSE(endpoint.lastError().has_value());

    endpoint.close();
    client.disconnectFromDaemon();
    REQUIRE_FALSE(endpoint.listen().has_value());
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    CHECK(std::holds_alternative<ReadyReply>(
        client.hello({.protocol = {.major = 1, .minor = 0},
                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                .revision = QStringLiteral("test")}})));
    CHECK_FALSE(client.ping().has_value());
}

TEST_CASE("socket endpoint admits every onboarding remote action", "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    RecordingHandler handler;
    const auto options = socketOptions(runtimeDirectory);
    SocketEndpointThread endpoint{handler, options};
    REQUIRE_FALSE(endpoint.listen().has_value());

    SocketDaemonClient client{options};
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    REQUIRE(std::holds_alternative<ReadyReply>(
        client.hello({.protocol = {.major = 1, .minor = 0},
                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                .revision = QStringLiteral("test")}})));

    const std::array actionIds{
        ActionId{.value = 67}, ActionId{.value = 68}, ActionId{.value = 69},
        ActionId{.value = 70}, ActionId{.value = 71}, ActionId{.value = 72},
    };
    for (const auto action : actionIds)
    {
        const auto payload = QByteArrayLiteral("onboarding-payload");
        const auto reply = client.submitCommand({
            .id = {.value = QUuid::createUuid()},
            .command = RemoteActionCommand{.action = action, .payload = payload},
        });
        const auto* accepted = std::get_if<CommandAccepted>(&reply);
        REQUIRE(accepted != nullptr);
        REQUIRE(accepted->immediateResult.has_value());
        CHECK(*accepted->immediateResult == payload);
    }
}

TEST_CASE("socket boundary events permit synchronous follow-up requests", "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    RecordingHandler handler;
    auto options = socketOptions(runtimeDirectory);
    options.responseTimeoutMilliseconds = 100;
    SocketEndpointThread endpoint{handler, options};
    REQUIRE_FALSE(endpoint.listen().has_value());

    SocketDaemonClient client{options};
    ReentrantRequestSink sink{client};
    REQUIRE_FALSE(client.attachEventSink(sink).has_value());
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    REQUIRE(std::holds_alternative<ReadyReply>(
        client.hello({.protocol = {.major = 1, .minor = 0},
                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                .revision = QStringLiteral("test")}})));

    endpoint.publishEvent(CacheInvalidation{
        .epoch = {.value = 13}, .changedDomains = {ChangedDomain::History}, .affectedKeys = {}});
    processUntil([&sink] { return sink.settings.has_value(); });

    REQUIRE(sink.received.has_value());
    REQUIRE(sink.settings.has_value());
    CHECK(std::holds_alternative<SettingsSnapshotReply>(*sink.settings));
    CHECK(client.isConnected());
}

TEST_CASE("socket command admission does not block the client event loop", "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    RecordingHandler handler;
    const auto options = socketOptions(runtimeDirectory);
    SocketEndpointThread endpoint{handler, options};
    REQUIRE_FALSE(endpoint.listen().has_value());

    SocketDaemonClient client{options};
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    REQUIRE(std::holds_alternative<ReadyReply>(
        client.hello({.protocol = {.major = 1, .minor = 0},
                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                .revision = QStringLiteral("test")}})));

    handler.onCommand = [] { QThread::msleep(150); };
    QElapsedTimer elapsed;
    elapsed.start();
    auto future = client.submitCommandAsync(refreshRequest());
    CHECK(elapsed.elapsed() < 50);
    CHECK_FALSE(future.isFinished());

    processUntil([&future] { return future.isFinished(); });
    REQUIRE(future.isFinished());
    CHECK(std::holds_alternative<CommandAccepted>(future.result()));
}

TEST_CASE("socket async command admission bounds outstanding replies", "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    RecordingHandler handler;
    auto options = socketOptions(runtimeDirectory);
    options.maximumQueuedFrames = 1;
    SocketEndpointThread endpoint{handler, options};
    REQUIRE_FALSE(endpoint.listen().has_value());

    SocketDaemonClient client{options};
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    REQUIRE(std::holds_alternative<ReadyReply>(
        client.hello({.protocol = {.major = 1, .minor = 0},
                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                .revision = QStringLiteral("test")}})));

    handler.onCommand = [] { QThread::msleep(150); };
    auto first = client.submitCommandAsync(refreshRequest());
    auto second = client.submitCommandAsync(refreshRequest());
    processUntil([&second] { return second.isFinished(); });
    REQUIRE(second.isFinished());
    const auto secondResult = second.result();
    const auto* rejected = std::get_if<CommandRejected>(&secondResult);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::Busy);

    processUntil([&first] { return first.isFinished(); });
    REQUIRE(first.isFinished());
    CHECK(std::holds_alternative<CommandAccepted>(first.result()));
}

TEST_CASE("activation route wire discriminators remain stable", "[protocol][socket][compatibility]")
{
    const std::vector<std::pair<ActivationRoute, quint8>> routes{
        {RestoreDraftRoute{}, 5},
        {OpenTaskCenterRoute{}, 6},
        {OpenMailtoRoute{}, 7},
        {ShowUndoSendDialogRoute{}, 8},
        {CloseUndoSendDialogRoute{}, 9},
        {OpenCalendarEventRoute{}, 10},
    };

    for (const auto& [route, expectedKind] : routes)
    {
        const auto encoded = encodeActivationRoute(route);
        REQUIRE(std::holds_alternative<QByteArray>(encoded));
        const auto& payload = std::get<QByteArray>(encoded);
        REQUIRE_FALSE(payload.isEmpty());
        CHECK(static_cast<quint8>(payload.front()) == expectedKind);
    }
}

TEST_CASE("activation socket carries typed routes to the daemon", "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    RecordingHandler handler;
    auto options = socketOptions(runtimeDirectory);
    options.socketPath += QStringLiteral(".activation");
    SocketActivationEndpointThread endpoint{handler, options};
    const auto listenError = endpoint.listen();
    REQUIRE_FALSE(listenError.has_value());

    const auto route =
        ActivationRoute{OpenMessageRoute{.accountId = QStringLiteral("account-1"),
                                         .mailboxId = QStringLiteral("mailbox-1"),
                                         .mailboxName = QStringLiteral("Projects"),
                                         .threadId = QStringLiteral("thread-1"),
                                         .emailId = QStringLiteral("email-1"),
                                         .activationToken = QStringLiteral("token-1")}};
    const auto result = SocketActivationClient::request(options, route);
    REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(result));
    CHECK_FALSE(std::get<std::optional<BoundaryError>>(result).has_value());
    const auto activatedRoute = handler.activatedRouteSnapshot();
    REQUIRE(activatedRoute.has_value());
    const auto* received = std::get_if<OpenMessageRoute>(&*activatedRoute);
    REQUIRE(received != nullptr);
    CHECK(received->accountId == QStringLiteral("account-1"));
    CHECK(received->mailboxId == QStringLiteral("mailbox-1"));
    CHECK(received->mailboxName == QStringLiteral("Projects"));
    CHECK(received->threadId == QStringLiteral("thread-1"));
    CHECK(received->emailId == QStringLiteral("email-1"));
    CHECK(received->activationToken == QStringLiteral("token-1"));

    const auto mailtoResult = SocketActivationClient::request(
        options, ActivationRoute{OpenMailtoRoute{
                     .uri = QStringLiteral("mailto:alice@example.test?subject=Hello%20there"),
                     .activationToken = QStringLiteral("token-mailto")}});
    REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(mailtoResult));
    CHECK_FALSE(std::get<std::optional<BoundaryError>>(mailtoResult).has_value());
    const auto mailtoRoute = handler.activatedRouteSnapshot();
    REQUIRE(mailtoRoute.has_value());
    const auto* receivedMailto = std::get_if<OpenMailtoRoute>(&*mailtoRoute);
    REQUIRE(receivedMailto != nullptr);
    CHECK(receivedMailto->uri == QStringLiteral("mailto:alice@example.test?subject=Hello%20there"));
    CHECK(receivedMailto->activationToken == QStringLiteral("token-mailto"));

    const auto calendarResult = SocketActivationClient::request(
        options, ActivationRoute{
                     OpenCalendarEventRoute{.calendarAccountId = QStringLiteral("calendar-account"),
                                            .eventId = QStringLiteral("event-42"),
                                            .recurrenceId = QStringLiteral("2026-08-21T09:30:00"),
                                            .navigationDate = QStringLiteral("2026-08-21"),
                                            .activationToken = QStringLiteral("token-calendar")}});
    REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(calendarResult));
    CHECK_FALSE(std::get<std::optional<BoundaryError>>(calendarResult).has_value());
    const auto calendarRoute = handler.activatedRouteSnapshot();
    REQUIRE(calendarRoute.has_value());
    const auto* receivedCalendar = std::get_if<OpenCalendarEventRoute>(&*calendarRoute);
    REQUIRE(receivedCalendar != nullptr);
    CHECK(receivedCalendar->calendarAccountId == QStringLiteral("calendar-account"));
    CHECK(receivedCalendar->eventId == QStringLiteral("event-42"));
    CHECK(receivedCalendar->recurrenceId ==
          std::optional<QString>{QStringLiteral("2026-08-21T09:30:00")});
    CHECK(receivedCalendar->navigationDate == QStringLiteral("2026-08-21"));
    CHECK(receivedCalendar->activationToken == QStringLiteral("token-calendar"));

    const auto undoDialogResult = SocketActivationClient::request(
        options, ActivationRoute{ShowUndoSendDialogRoute{
                     .sendId = QStringLiteral("send-1"),
                     .title = QStringLiteral("Message scheduled"),
                     .message = QStringLiteral("Send “Quarterly report”"),
                     .deadlineEpochMilliseconds = 1'800'000'000'123,
                 }});
    REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(undoDialogResult));
    CHECK_FALSE(std::get<std::optional<BoundaryError>>(undoDialogResult).has_value());
    const auto undoDialogRoute = handler.activatedRouteSnapshot();
    REQUIRE(undoDialogRoute.has_value());
    const auto* receivedUndoDialog = std::get_if<ShowUndoSendDialogRoute>(&*undoDialogRoute);
    REQUIRE(receivedUndoDialog != nullptr);
    CHECK(receivedUndoDialog->sendId == QStringLiteral("send-1"));
    CHECK(receivedUndoDialog->title == QStringLiteral("Message scheduled"));
    CHECK(receivedUndoDialog->message == QStringLiteral("Send “Quarterly report”"));
    CHECK(receivedUndoDialog->deadlineEpochMilliseconds == 1'800'000'000'123);

    for (int attempt = 0; attempt < 16; ++attempt)
    {
        const auto repeated =
            SocketActivationClient::request(options, ActivationRoute{RaiseGuiRoute{}});
        REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(repeated));
        CHECK_FALSE(std::get<std::optional<BoundaryError>>(repeated).has_value());
    }

    auto incompatibleOptions = options;
    incompatibleOptions.expectedBuild = BuildIdentity{.application = QStringLiteral("Javelin-Mail"),
                                                      .revision = QStringLiteral("other-build")};
    const auto incompatible =
        SocketActivationClient::request(incompatibleOptions, ActivationRoute{RaiseGuiRoute{}});
    REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(incompatible));
    const auto& incompatibleError = std::get<std::optional<BoundaryError>>(incompatible);
    REQUIRE(incompatibleError.has_value());
    CHECK(incompatibleError->code == BoundaryErrorCode::IncompatibleBuild);

    handler.setActivationError(BoundaryError{.code = BoundaryErrorCode::Busy,
                                             .field = QStringLiteral("activation"),
                                             .detail = QStringLiteral("GUI is already active")});
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const auto rejected =
        SocketActivationClient::request(options, ActivationRoute{RaiseGuiRoute{}});
    REQUIRE(std::holds_alternative<std::optional<BoundaryError>>(rejected));
    const auto& error = std::get<std::optional<BoundaryError>>(rejected);
    REQUIRE(error.has_value());
    CHECK(error->code == BoundaryErrorCode::Busy);
}

TEST_CASE("socket endpoint rejects malformed peers and classifies lost replies",
          "[protocol][socket]")
{
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());
    RecordingHandler handler;
    const auto options = socketOptions(runtimeDirectory);
    SocketEndpointThread endpoint{handler, options};
    const auto listenError = endpoint.listen();
    REQUIRE_FALSE(listenError.has_value());

    QLocalSocket raw;
    raw.connectToServer(options.socketPath);
    REQUIRE(raw.waitForConnected(2000));
    QByteArray unknown;
    const auto valid = encodeSocketFrame(SocketFrameKind::PingRequest, 1, {});
    REQUIRE(std::holds_alternative<QByteArray>(valid));
    unknown = std::get<QByteArray>(valid);
    unknown[6] = static_cast<char>(0x7f);
    unknown[7] = static_cast<char>(0xff);
    REQUIRE(raw.write(unknown) == unknown.size());
    REQUIRE(raw.waitForReadyRead(2000));
    SocketFrameDecoder decoder;
    REQUIRE_FALSE(decoder.append(raw.readAll()).has_value());
    const auto errorFrame = decoder.takeFrame();
    REQUIRE(std::holds_alternative<std::optional<SocketFrame>>(errorFrame));
    REQUIRE(std::get<std::optional<SocketFrame>>(errorFrame).has_value());
    CHECK(std::get<std::optional<SocketFrame>>(errorFrame)->kind == SocketFrameKind::ProtocolError);
    raw.abort();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QLocalSocket malformedPeer;
    malformedPeer.connectToServer(options.socketPath);
    REQUIRE(malformedPeer.waitForConnected(2000));
    QByteArray invalidCommandPayload(17, '\0');
    invalidCommandPayload[16] = static_cast<char>(9);
    const auto invalidCommand =
        encodeSocketFrame(SocketFrameKind::CommandRequest, 2, invalidCommandPayload);
    REQUIRE(std::holds_alternative<QByteArray>(invalidCommand));
    const auto invalidCommandFrame = std::get<QByteArray>(invalidCommand);
    REQUIRE(malformedPeer.write(invalidCommandFrame) == invalidCommandFrame.size());
    REQUIRE(malformedPeer.waitForReadyRead(2000));
    SocketFrameDecoder malformedPeerDecoder;
    REQUIRE_FALSE(malformedPeerDecoder.append(malformedPeer.readAll()).has_value());
    const auto malformedPeerReply = malformedPeerDecoder.takeFrame();
    REQUIRE(std::holds_alternative<std::optional<SocketFrame>>(malformedPeerReply));
    REQUIRE(std::get<std::optional<SocketFrame>>(malformedPeerReply).has_value());
    CHECK(std::get<std::optional<SocketFrame>>(malformedPeerReply)->kind ==
          SocketFrameKind::ProtocolError);
    malformedPeer.abort();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    SocketDaemonClient client{options};
    REQUIRE_FALSE(client.connectToDaemon().has_value());
    REQUIRE(std::holds_alternative<ReadyReply>(
        client.hello({.protocol = {.major = 1, .minor = 0},
                      .build = {.application = QStringLiteral("Javelin-Mail"),
                                .revision = QStringLiteral("test")}})));
    SocketDaemonEndpoint* endpointPointer = endpoint.endpointForThreadCallback();
    handler.onCommand = [endpointPointer] { endpointPointer->close(); };
    const auto lostReply = client.submitCommand(refreshRequest());
    REQUIRE(std::holds_alternative<CommandRejected>(lostReply));
    CHECK(std::get<CommandRejected>(lostReply).error.code ==
          BoundaryErrorCode::TransportUnavailable);
    CHECK_FALSE(client.isConnected());
}
