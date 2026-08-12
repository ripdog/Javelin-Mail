#include "app/DaemonTrayController.h"
#include "app/WorkScheduler.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoreApplication>
#include <QDBusMetaType>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "daemon-tray-controller-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }

    void processEventsFor(const std::chrono::milliseconds duration)
    {
        QEventLoop loop;
        QTimer::singleShot(duration, &loop, &QEventLoop::quit);
        loop.exec();
    }
} // namespace

TEST_CASE("daemon tray tooltip reports unread inbox mail and only running work",
          "[app][daemon-tray]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("daemon-tray-controller-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
    javelin::app::DaemonTrayController tray{scheduler, std::chrono::milliseconds{0}};

    tray.setInboxUnreadCount(3);
    CHECK(tray.toolTip().title == QStringLiteral("3 unread emails in Inbox"));
    CHECK(tray.toolTip().description.isEmpty());

    REQUIRE_FALSE(scheduler
                      .ensure({.jobId = "sync",
                               .parentJobId = std::nullopt,
                               .accountId = std::nullopt,
                               .kind = javelin::app::WorkKind::FullMailSync,
                               .priority = javelin::app::WorkPriority::Bulk,
                               .title = QStringLiteral("Download Archive"),
                               .checkpointJson = QStringLiteral("{}"),
                               .restartCompleted = false})
                      .has_value());
    REQUIRE_FALSE(scheduler
                      .update("sync", javelin::app::WorkStatus::Running,
                              {.completedUnits = 25,
                               .totalUnits = 100,
                               .completedBytes = 0,
                               .totalBytes = std::nullopt,
                               .detail = QStringLiteral("Downloading")})
                      .has_value());
    CHECK(tray.toolTip().description ==
          QStringLiteral("Download Archive — Downloading — 25 / 100"));

    REQUIRE_FALSE(scheduler.pause("sync").has_value());
    CHECK(tray.toolTip().description.isEmpty());
}

TEST_CASE("daemon tray coalesces tooltip updates to the configured interval", "[app][daemon-tray]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("daemon-tray-tooltip-throttle-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
    javelin::app::DaemonTrayController tray{scheduler, std::chrono::milliseconds{50}};

    tray.setInboxUnreadCount(1);
    CHECK(tray.toolTip().title == QStringLiteral("1 unread email in Inbox"));

    tray.setInboxUnreadCount(2);
    tray.setInboxUnreadCount(3);
    CHECK(tray.toolTip().title == QStringLiteral("1 unread email in Inbox"));

    processEventsFor(std::chrono::milliseconds{70});
    CHECK(tray.toolTip().title == QStringLiteral("3 unread emails in Inbox"));

    tray.setInboxUnreadCount(4);
    CHECK(tray.toolTip().title == QStringLiteral("3 unread emails in Inbox"));
    processEventsFor(std::chrono::milliseconds{60});
    CHECK(tray.toolTip().title == QStringLiteral("4 unread emails in Inbox"));
}

TEST_CASE("daemon tray exports the StatusNotifierItem tooltip DBus signature", "[app][daemon-tray]")
{
    ensureApplication();
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("daemon-tray-dbus-signature-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
    javelin::app::DaemonTrayController tray{scheduler};
    Q_UNUSED(tray);

    CHECK(QString::fromLatin1(QDBusMetaType::typeToSignature(
              QMetaType::fromType<javelin::app::TrayToolTip>())) == QStringLiteral("(sa(iiay)ss)"));
}
