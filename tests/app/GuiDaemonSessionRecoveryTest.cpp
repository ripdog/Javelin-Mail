#include "client/GuiDaemonSession.h"
#include "protocol/LocalDaemonServer.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <variant>

namespace
{
    using namespace javelin::protocol;

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
                return;
            static int argc = 1;
            static char applicationName[] = "javelin-gui-daemon-session-recovery-tests";
            static char* argv[] = {applicationName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    enum class SlowBootstrapStage
    {
        Hello,
        Settings,
        Activation,
    };

    class SlowBootstrapHandler final : public DaemonRequestHandler
    {
      public:
        SlowBootstrapHandler(QString cachePath, const SlowBootstrapStage slowStage)
            : m_cachePath(std::move(cachePath)), m_slowStage(slowStage)
        {
        }

        HandshakeReply handleHello(const HelloRequest&) override
        {
            if (helloCount.fetch_add(1) == 0 && m_slowStage == SlowBootstrapStage::Hello)
                QThread::msleep(150);
            return ReadyReply{.protocol = {.major = 5, .minor = 11},
                              .daemon = {.value = QUuid::createUuid()},
                              .cache = {.instance = {.value = QUuid::createUuid()},
                                        .schema = {.value = 3},
                                        .dataVersion = {.value = 1}},
                              .cacheDatabasePath = m_cachePath,
                              .epoch = {.value = 1},
                              .settingsRevision = {.value = 1}};
        }

        CommandReply handleCommand(CommandRequest request) override
        {
            return CommandAccepted{.id = request.id,
                                   .operation = std::nullopt,
                                   .epoch = {.value = 1},
                                   .changedDomains = {},
                                   .affectedKeys = {},
                                   .immediateResult = std::nullopt};
        }

        MaterializationReply handleMaterialization(MaterializationRequest request) override
        {
            return MaterializationAccepted{.id = request.id};
        }

        void handleCancelMaterializationScope(const CancelMaterializationScopeRequest&) override
        {
        }

        SettingsReadReply handleGetSettings(const GetSettingsRequest&) override
        {
            if (settingsCount.fetch_add(1) == 0 && m_slowStage == SlowBootstrapStage::Settings)
                QThread::msleep(150);
            return SettingsSnapshotReply{
                .snapshot = {
                    .revision = {.value = 1},
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
                    .workspace =
                        {
                            .formatVersion = 1,
                            .mainWindowState = QByteArrayLiteral("window-state"),
                            .composeRichTextDefault = false,
                            .defaultCalendarDestination = {.ownerAccountId =
                                                               QStringLiteral("server-1"),
                                                           .accountId = QStringLiteral("account-1"),
                                                           .calendarId =
                                                               QStringLiteral("calendar-default")},
                            .emailContextMenuLayout = {},
                            .calendarEventContextMenuLayout = {},
                        },
                }};
        }

        SettingsUpdateReply handleUpdateSettings(UpdateSettingsRequest request) override
        {
            return SettingsUpdated{.revision = {.value = request.baseRevision.value + 1}};
        }

        std::optional<BoundaryError>
        handleCacheAccessSuspended(const CacheAccessSuspendedAcknowledgement&) override
        {
            return std::nullopt;
        }

        std::optional<BoundaryError> handlePing(const PingRequest&) override
        {
            return std::nullopt;
        }

        std::optional<BoundaryError> handleGuiReadyForActivation() override
        {
            if (activationCount.fetch_add(1) == 0 && m_slowStage == SlowBootstrapStage::Activation)
            {
                QThread::msleep(150);
            }
            guiReady.store(true);
            return std::nullopt;
        }

        std::atomic_int helloCount = 0;
        std::atomic_int settingsCount = 0;
        std::atomic_int activationCount = 0;
        std::atomic_bool guiReady = false;

      private:
        QString m_cachePath;
        SlowBootstrapStage m_slowStage = SlowBootstrapStage::Hello;
    };

    class SocketEndpointThread final
    {
      public:
        SocketEndpointThread(DaemonRequestHandler& handler, SocketEndpointOptions options)
            : m_endpoint(new SocketDaemonEndpoint(handler, std::move(options)))
        {
            m_endpoint->moveToThread(&m_thread);
            m_thread.start();
        }

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

      private:
        QThread m_thread;
        SocketDaemonEndpoint* m_endpoint = nullptr;
    };

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
} // namespace

TEST_CASE("GUI bootstrap reports an absent daemon as unavailable immediately",
          "[app][gui][recovery]")
{
    ApplicationGuard application;
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    javelin::app::GuiDaemonSession session{
        {.runtimeDirectory = runtimeDirectory.path(),
         .socketPath = QDir{runtimeDirectory.path()}.filePath(QStringLiteral("missing.sock")),
         .daemonExecutable = {},
         .protocol = {.major = 5, .minor = 11},
         .build = {.application = QStringLiteral("Javelin-Mail"),
                   .revision = QStringLiteral("test")},
         .startTimeoutMilliseconds = 50,
         .responseTimeoutMilliseconds = 50,
         .startDaemonIfMissing = false}};

    const auto error = session.start();
    REQUIRE(error.has_value());
    CHECK(error->code == javelin::app::GuiBootstrapErrorCode::DaemonUnavailable);
    CHECK_FALSE(session.isReady());
}

TEST_CASE("GUI bootstrap automatically resumes the exact late daemon reply at every stage",
          "[app][gui][recovery]")
{
    SlowBootstrapStage slowStage = SlowBootstrapStage::Hello;
    SECTION("Hello")
    {
        slowStage = SlowBootstrapStage::Hello;
    }
    SECTION("settings")
    {
        slowStage = SlowBootstrapStage::Settings;
    }
    SECTION("activation")
    {
        slowStage = SlowBootstrapStage::Activation;
    }

    ApplicationGuard application;
    QTemporaryDir runtimeDirectory;
    REQUIRE(runtimeDirectory.isValid());

    const auto cachePath =
        QDir{runtimeDirectory.path()}.filePath(QStringLiteral("javelin-cache.sqlite3"));
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("gui-recovery-writer-%1")
                                  .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)),
            .databasePath = cachePath,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
        {
            FAIL(error->message.toStdString());
        }
    }

    const auto socketPath = QDir{runtimeDirectory.path()}.filePath(QStringLiteral("javelind.sock"));
    SlowBootstrapHandler handler{cachePath, slowStage};
    SocketEndpointThread endpoint{
        handler,
        {.runtimeDirectory = runtimeDirectory.path(),
         .socketPath = socketPath,
         .limits = {},
         .protocol = {.major = 5, .minor = 11},
         .expectedBuild = BuildIdentity{.application = QStringLiteral("Javelin-Mail"),
                                        .revision = QStringLiteral("test")},
         .maximumQueuedFrames = 8,
         .maximumQueuedBytes = 4 * 1024 * 1024,
         .responseTimeoutMilliseconds = 2000,
         .enforcePeerCredentials = true}};
    REQUIRE_FALSE(endpoint.listen().has_value());

    javelin::app::GuiDaemonSession session{{.runtimeDirectory = runtimeDirectory.path(),
                                            .socketPath = socketPath,
                                            .daemonExecutable = {},
                                            .protocol = {.major = 5, .minor = 11},
                                            .build = {.application = QStringLiteral("Javelin-Mail"),
                                                      .revision = QStringLiteral("test")},
                                            .startTimeoutMilliseconds = 50,
                                            .responseTimeoutMilliseconds = 50,
                                            .startDaemonIfMissing = false}};

    bool ready = false;
    bool recovered = false;
    QObject::connect(&session, &javelin::app::GuiDaemonSession::ready, [&ready] { ready = true; });
    QObject::connect(&session, &javelin::app::GuiDaemonSession::recoveryFinished,
                     [&recovered] { recovered = true; });

    const auto startError = session.start();
    REQUIRE(startError.has_value());
    CHECK(startError->code == javelin::app::GuiBootstrapErrorCode::DaemonBusy);
    CHECK_FALSE(session.isReady());
    CHECK(handler.helloCount.load() == 1);

    processUntil([&] { return recovered; });

    CHECK(recovered);
    CHECK(ready);
    CHECK(session.isReady());
    CHECK(handler.guiReady.load());
    CHECK(handler.helloCount.load() == 1);
    CHECK(handler.settingsCount.load() == 1);
    CHECK(handler.activationCount.load() == 1);
}
