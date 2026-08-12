#include "app/CacheAccessBarrier.h"

#include "storage/migrations/MigrationRunner.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

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
            static char appName[] = "javelin-cache-barrier-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] QString connectionName()
    {
        static int counter = 0;
        return QStringLiteral("javelin-cache-barrier-%1").arg(++counter);
    }
} // namespace

TEST_CASE("cache access barrier suspends and resumes every participant")
{
    javelin::app::CacheAccessBarrier barrier;
    std::vector<int> events;
    static_cast<void>(barrier.registerParticipant({
        .name = QStringLiteral("GUI snapshot"),
        .suspend =
            [&events]
        {
            events.push_back(1);
            return std::nullopt;
        },
        .resume =
            [&events]
        {
            events.push_back(4);
            return std::nullopt;
        },
    }));
    static_cast<void>(barrier.registerParticipant({
        .name = QStringLiteral("vault lease"),
        .suspend =
            [&events]
        {
            events.push_back(2);
            return std::nullopt;
        },
        .resume =
            [&events]
        {
            events.push_back(3);
            return std::nullopt;
        },
    }));

    REQUIRE_FALSE(barrier.isSuspended());
    REQUIRE_FALSE(barrier.suspend().has_value());
    REQUIRE(barrier.isSuspended());
    REQUIRE(events == std::vector<int>{1, 2});
    REQUIRE_FALSE(barrier.suspend().has_value());
    REQUIRE_FALSE(barrier.resume().has_value());
    REQUIRE_FALSE(barrier.isSuspended());
    REQUIRE(events == std::vector<int>{1, 2, 4, 3});
}

TEST_CASE("cache access barrier rolls back earlier suspends after a failure")
{
    javelin::app::CacheAccessBarrier barrier;
    int resumed = 0;
    static_cast<void>(barrier.registerParticipant({
        .name = QStringLiteral("first"),
        .suspend = [] { return std::nullopt; },
        .resume =
            [&resumed]
        {
            ++resumed;
            return std::nullopt;
        },
    }));
    static_cast<void>(barrier.registerParticipant({
        .name = QStringLiteral("failing"),
        .suspend =
            []
        {
            return std::optional<javelin::jmap::cache::DatabaseError>{
                {.code = javelin::jmap::cache::DatabaseErrorCode::QueryFailed,
                 .message = QStringLiteral("test failure")}};
        },
        .resume = [] { return std::nullopt; },
    }));

    REQUIRE(barrier.suspend().has_value());
    REQUIRE_FALSE(barrier.isSuspended());
    REQUIRE(resumed == 1);
}

TEST_CASE("cache access barrier reopens a read-only cache after replacement")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const auto databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));

    auto daemonResult =
        javelin::jmap::cache::DaemonDatabaseFactory{
            javelin::jmap::cache::DatabaseConnectionOptions{.connectionName = connectionName(),
                                                            .databasePath = databasePath}}
            .open();
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(daemonResult));
    auto daemon = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(daemonResult));

    auto guiResult =
        javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-cache-barrier-read"),
                .databasePath = databasePath}}
            .openForCurrentThread("active-view");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(guiResult));
    std::optional<javelin::jmap::cache::ReadOnlyDatabaseConnection> gui{
        std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(guiResult))};

    javelin::app::CacheAccessBarrier barrier;
    static_cast<void>(barrier.registerParticipant({
        .name = QStringLiteral("GUI read connection"),
        .suspend =
            [&gui]
        {
            gui.reset();
            return std::nullopt;
        },
        .resume = [&gui, &databasePath]() -> std::optional<javelin::jmap::cache::DatabaseError>
        {
            auto reopened =
                javelin::jmap::cache::GuiDatabaseFactory{
                    javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                        .connectionNamePrefix = QStringLiteral("javelin-cache-barrier-read"),
                        .databasePath = databasePath}}
                    .openForCurrentThread("active-view");
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&reopened))
                return *error;
            gui.emplace(
                std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(reopened)));
            return std::nullopt;
        },
    }));

    REQUIRE_FALSE(barrier.suspend().has_value());
    REQUIRE_FALSE(gui.has_value());
    daemon = {};
    REQUIRE(QFile::remove(databasePath));
    auto replacementResult = javelin::jmap::cache::DaemonDatabaseFactory{
        javelin::jmap::cache::DatabaseConnectionOptions{
            .connectionName = connectionName(),
            .databasePath =
                databasePath}}.open();
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(replacementResult));
    daemon = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(replacementResult));

    REQUIRE_FALSE(barrier.resume().has_value());
    REQUIRE(gui.has_value());
    CHECK(gui->schemaVersion() ==
          javelin::jmap::cache::createDefaultMigrationRunner().latestVersion());
    CHECK(std::holds_alternative<std::uint64_t>(gui->dataVersion()));
}
