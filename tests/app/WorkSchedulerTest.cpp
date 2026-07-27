#include "app/WorkScheduler.h"
#include "jmap/cache/Database.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>

TEST_CASE("work scheduler recovers running work and preserves explicit pauses",
          "[app][work-scheduler]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    {
        javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
        REQUIRE_FALSE(scheduler
                          .ensure({.jobId = "job-1",
                                   .parentJobId = std::nullopt,
                                   .accountId = std::nullopt,
                                   .kind = javelin::app::WorkKind::FullMailSync,
                                   .priority = javelin::app::WorkPriority::Bulk,
                                   .title = QStringLiteral("Download mail"),
                                   .checkpointJson = QStringLiteral("{\"position\":250}")})
                          .has_value());
        REQUIRE_FALSE(scheduler
                          .update("job-1", javelin::app::WorkStatus::Running,
                                  {.completedUnits = 250,
                                   .totalUnits = 1000,
                                   .completedBytes = 4096,
                                   .totalBytes = 16384,
                                   .detail = QStringLiteral("Downloading")},
                                  QStringLiteral("{\"position\":250}"))
                          .has_value());
    }

    javelin::app::WorkScheduler recovered{connection, nullptr, std::chrono::milliseconds{0}};
    auto record = recovered.find("job-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::app::WorkRecord>>(record));
    REQUIRE(std::get<std::optional<javelin::app::WorkRecord>>(record).has_value());
    CHECK(std::get<std::optional<javelin::app::WorkRecord>>(record)->status ==
          javelin::app::WorkStatus::Queued);
    CHECK(std::get<std::optional<javelin::app::WorkRecord>>(record)->progress.completedUnits ==
          250);

    REQUIRE_FALSE(recovered.pause("job-1").has_value());
    javelin::app::WorkScheduler pausedRecovery{connection, nullptr, std::chrono::milliseconds{0}};
    record = pausedRecovery.find("job-1");
    REQUIRE(std::get<std::optional<javelin::app::WorkRecord>>(record).has_value());
    CHECK(std::get<std::optional<javelin::app::WorkRecord>>(record)->status ==
          javelin::app::WorkStatus::Paused);
    CHECK(std::get<std::optional<javelin::app::WorkRecord>>(record)->pauseRequested);

    CHECK(pausedRecovery.mayStartBackgroundNetwork());
    pausedRecovery.beginForegroundWork();
    CHECK_FALSE(pausedRecovery.mayStartBackgroundNetwork());
    pausedRecovery.endForegroundWork();
    CHECK(pausedRecovery.mayStartBackgroundNetwork());
}

TEST_CASE("work scheduler requeues configured failed work and preserves checkpoints",
          "[app][work-scheduler]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-failed-retry-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-failed-retry-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};

    const javelin::app::WorkSpec spec{
        .jobId = "failed-job",
        .parentJobId = std::nullopt,
        .accountId = "account-1",
        .kind = javelin::app::WorkKind::FullMailSync,
        .priority = javelin::app::WorkPriority::Bulk,
        .title = QStringLiteral("Download archive"),
        .checkpointJson = QStringLiteral("{}"),
    };
    REQUIRE_FALSE(scheduler.ensure(spec).has_value());
    REQUIRE_FALSE(scheduler
                      .update("failed-job", javelin::app::WorkStatus::Failed,
                              {.completedUnits = 100,
                               .totalUnits = 41391,
                               .completedBytes = 0,
                               .totalBytes = std::nullopt,
                               .detail = QStringLiteral("Reading mailbox contents")},
                              QStringLiteral("{\"generation\":3,\"position\":100}"),
                              QStringLiteral("database is locked"))
                      .has_value());

    REQUIRE_FALSE(scheduler.ensure(spec).has_value());
    const auto retried = scheduler.find("failed-job");
    REQUIRE(std::holds_alternative<std::optional<javelin::app::WorkRecord>>(retried));
    const auto& retriedRecord = std::get<std::optional<javelin::app::WorkRecord>>(retried);
    REQUIRE(retriedRecord.has_value());
    CHECK(retriedRecord->status == javelin::app::WorkStatus::Queued);
    CHECK(retriedRecord->progress.completedUnits == 100);
    CHECK(retriedRecord->progress.totalUnits == std::optional<std::uint64_t>{41391});
    CHECK(retriedRecord->checkpointJson == QStringLiteral("{\"generation\":3,\"position\":100}"));
    CHECK_FALSE(retriedRecord->errorText.has_value());

    REQUIRE_FALSE(scheduler.pause("failed-job").has_value());
    REQUIRE_FALSE(scheduler.ensure(spec).has_value());
    const auto paused = scheduler.find("failed-job");
    REQUIRE(std::holds_alternative<std::optional<javelin::app::WorkRecord>>(paused));
    const auto& pausedRecord = std::get<std::optional<javelin::app::WorkRecord>>(paused);
    REQUIRE(pausedRecord.has_value());
    CHECK(pausedRecord->status == javelin::app::WorkStatus::Paused);
    CHECK(pausedRecord->pauseRequested);
}

TEST_CASE("work scheduler observes a quiet period after startup and foreground work",
          "[app][work-scheduler]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-quiet-period-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-quiet-period-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::seconds{5}};
    CHECK_FALSE(scheduler.mayStartBackgroundNetwork());
    scheduler.beginForegroundWork();
    scheduler.endForegroundWork();
    CHECK_FALSE(scheduler.mayStartBackgroundNetwork());
}

TEST_CASE("work scheduler atomically restarts completed refresh work", "[app][work-scheduler]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-completed-refresh-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-completed-refresh-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
    const javelin::app::WorkSpec spec{
        .jobId = "contact-refresh:a1",
        .parentJobId = std::nullopt,
        .accountId = "a1",
        .kind = javelin::app::WorkKind::ContactRefresh,
        .priority = javelin::app::WorkPriority::Freshness,
        .title = QStringLiteral("Refresh contacts"),
        .checkpointJson = QStringLiteral("{}"),
        .restartCompleted = true,
    };
    REQUIRE_FALSE(scheduler.ensure(spec).has_value());
    REQUIRE_FALSE(scheduler
                      .update(spec.jobId, javelin::app::WorkStatus::Complete,
                              {.completedUnits = 10,
                               .totalUnits = 10,
                               .completedBytes = 0,
                               .totalBytes = std::nullopt,
                               .detail = QStringLiteral("Complete")},
                              QStringLiteral("{}"))
                      .has_value());

    REQUIRE_FALSE(scheduler.ensure(spec).has_value());
    const auto restarted = scheduler.find(spec.jobId);
    REQUIRE(std::holds_alternative<std::optional<javelin::app::WorkRecord>>(restarted));
    REQUIRE(std::get<std::optional<javelin::app::WorkRecord>>(restarted).has_value());
    CHECK(std::get<std::optional<javelin::app::WorkRecord>>(restarted)->status ==
          javelin::app::WorkStatus::Queued);
}
