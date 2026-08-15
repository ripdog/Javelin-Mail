#include "app/WorkScheduler.h"
#include "storage/sqlite/DatabaseConnection.h"

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

TEST_CASE("work scheduler preserves tag deletion jobs across restart",
          "[app][work-scheduler][tags]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-tag-deletion-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-tag-deletion-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    {
        javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
        REQUIRE_FALSE(scheduler
                          .ensure({.jobId = "tag-delete:abc",
                                   .parentJobId = std::nullopt,
                                   .accountId = "account-1",
                                   .kind = javelin::app::WorkKind::TagDeletion,
                                   .priority = javelin::app::WorkPriority::Bulk,
                                   .title = QStringLiteral("Delete tag Project X"),
                                   .checkpointJson = QStringLiteral("{\"keyword\":\"project-x\"}"),
                                   .restartCompleted = true})
                          .has_value());
        REQUIRE_FALSE(scheduler
                          .update("tag-delete:abc", javelin::app::WorkStatus::Running,
                                  {.completedUnits = 25,
                                   .totalUnits = 50,
                                   .completedBytes = 0,
                                   .totalBytes = std::nullopt,
                                   .detail = QStringLiteral("Removing tag")},
                                  QStringLiteral("{\"keyword\":\"project-x\"}"))
                          .has_value());
    }

    javelin::app::WorkScheduler recovered{connection, nullptr, std::chrono::milliseconds{0}};
    const auto result = recovered.find("tag-delete:abc");
    REQUIRE(std::holds_alternative<std::optional<javelin::app::WorkRecord>>(result));
    const auto& job = std::get<std::optional<javelin::app::WorkRecord>>(result);
    REQUIRE(job.has_value());
    CHECK(job->kind == javelin::app::WorkKind::TagDeletion);
    CHECK(job->status == javelin::app::WorkStatus::Queued);
    CHECK(job->checkpointJson == QStringLiteral("{\"keyword\":\"project-x\"}"));
    CHECK(javelin::app::classify(job->kind) == javelin::app::WorkClass::Maintenance);
}

TEST_CASE("work scheduler preserves mail transfer jobs across restart",
          "[app][work-scheduler][mail-transfer]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-mail-transfer-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-mail-transfer-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    {
        javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};
        REQUIRE_FALSE(scheduler
                          .ensure({.jobId = "mail-transfer:operation-1",
                                   .parentJobId = std::nullopt,
                                   .accountId = "source-account",
                                   .kind = javelin::app::WorkKind::MailTransfer,
                                   .priority = javelin::app::WorkPriority::Foreground,
                                   .title = QStringLiteral("Move 2 messages"),
                                   .checkpointJson = QStringLiteral(
                                       "{\"operationId\":\"operation-1\",\"canRetry\":false}")})
                          .has_value());
        REQUIRE_FALSE(
            scheduler
                .update("mail-transfer:operation-1", javelin::app::WorkStatus::WaitingForNetwork,
                        {.completedUnits = 1,
                         .totalUnits = 2,
                         .completedBytes = 2048,
                         .totalBytes = 4096,
                         .detail = QStringLiteral("Waiting for network")},
                        QStringLiteral("{\"operationId\":\"operation-1\",\"canRetry\":false}"))
                .has_value());
    }

    javelin::app::WorkScheduler recovered{connection, nullptr, std::chrono::milliseconds{0}};
    const auto result = recovered.find("mail-transfer:operation-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::app::WorkRecord>>(result));
    const auto& job = std::get<std::optional<javelin::app::WorkRecord>>(result);
    REQUIRE(job.has_value());
    CHECK(job->kind == javelin::app::WorkKind::MailTransfer);
    CHECK(job->status == javelin::app::WorkStatus::WaitingForNetwork);
    CHECK(job->progress.completedUnits == 1);
    CHECK(job->progress.totalUnits == std::optional<std::uint64_t>{2});
    CHECK(javelin::app::classify(job->kind, job->priority) ==
          javelin::app::WorkClass::ForegroundCommand);
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

TEST_CASE("visible materialization skips quiet delay but not foreground work",
          "[app][work-scheduler][admission]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-visible-materialization-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-visible-materialization-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::seconds{5}};
    CHECK_FALSE(
        scheduler.admitTransient("freshness", "account-1", javelin::app::WorkPriority::Freshness)
            .has_value());
    const auto initialVisible = scheduler.admitTransient(
        "visible-initial", "account-1", javelin::app::WorkPriority::VisibleMaterialization);
    REQUIRE(initialVisible.has_value());
    scheduler.release("visible-initial");

    scheduler.beginForegroundWork();
    CHECK_FALSE(scheduler
                    .admitTransient("visible-during-foreground", "account-1",
                                    javelin::app::WorkPriority::VisibleMaterialization)
                    .has_value());
    const auto interactive = scheduler.admitTransient("interactive", "account-1",
                                                      javelin::app::WorkPriority::Interactive);
    REQUIRE(interactive.has_value());
    scheduler.release("interactive");
    scheduler.endForegroundWork();

    const auto visibleAfterForeground =
        scheduler.admitTransient("visible-after-foreground", "account-1",
                                 javelin::app::WorkPriority::VisibleMaterialization);
    REQUIRE(visibleAfterForeground.has_value());
    scheduler.release("visible-after-foreground");
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

TEST_CASE("work scheduler admits priority work fairly across accounts",
          "[app][work-scheduler][admission]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-admission-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-admission-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};

    REQUIRE_FALSE(scheduler
                      .ensure({.jobId = "bulk-a",
                               .parentJobId = std::nullopt,
                               .accountId = "account-a",
                               .kind = javelin::app::WorkKind::FullMailSync,
                               .priority = javelin::app::WorkPriority::Bulk,
                               .title = QStringLiteral("Bulk A"),
                               .checkpointJson = QStringLiteral("{}")})
                      .has_value());
    REQUIRE_FALSE(scheduler
                      .ensure({.jobId = "fresh-b",
                               .parentJobId = std::nullopt,
                               .accountId = "account-b",
                               .kind = javelin::app::WorkKind::CalendarRefresh,
                               .priority = javelin::app::WorkPriority::Freshness,
                               .title = QStringLiteral("Fresh B"),
                               .checkpointJson = QStringLiteral("{}")})
                      .has_value());

    CHECK_FALSE(scheduler.admit("bulk-a").has_value());
    const auto freshAdmission = scheduler.admit("fresh-b");
    REQUIRE(freshAdmission.has_value());
    CHECK(scheduler.activeAdmissions() == 1);
    CHECK_FALSE(scheduler.admit("fresh-b").has_value());

    const auto bulkAdmission = scheduler.admit("bulk-a");
    REQUIRE(bulkAdmission.has_value());
    CHECK(scheduler.activeAdmissions() == 2);
    CHECK(scheduler.admissionMetrics().admitted == 2);

    scheduler.release("fresh-b");
    scheduler.release("bulk-a");
    CHECK(scheduler.activeAdmissions() == 0);
    CHECK(scheduler.admissionMetrics().completed == 2);
    CHECK(scheduler.admissionMetrics().maximumQueueWait.count() >= 0);
    scheduler.recordTransactionDuration(std::chrono::microseconds{17});
    scheduler.recordForegroundAdmissionLatency(std::chrono::microseconds{11});
    CHECK(scheduler.admissionMetrics().totalTransactionTime == std::chrono::microseconds{17});
    CHECK(scheduler.admissionMetrics().totalForegroundAdmissionLatency ==
          std::chrono::microseconds{11});
    CHECK(javelin::app::classify(javelin::app::WorkKind::SearchIndex) ==
          javelin::app::WorkClass::Indexing);
    CHECK(javelin::app::classify(javelin::app::WorkKind::FullMailSync) ==
          javelin::app::WorkClass::OfflineSynchronization);
    CHECK(javelin::app::classify(javelin::app::WorkKind::Maintenance,
                                 javelin::app::WorkPriority::Interactive) ==
          javelin::app::WorkClass::ForegroundCommand);
}

TEST_CASE("work scheduler bounds durable queued work", "[app][work-scheduler][queue]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-queue-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-queue-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};

    for (std::size_t index = 0; index < javelin::app::WorkScheduler::maximumQueuedWork; ++index)
    {
        REQUIRE_FALSE(scheduler
                          .ensure({.jobId = "bounded-" + std::to_string(index),
                                   .parentJobId = std::nullopt,
                                   .accountId = std::nullopt,
                                   .kind = javelin::app::WorkKind::Maintenance,
                                   .priority = javelin::app::WorkPriority::Maintenance,
                                   .title = QStringLiteral("Maintenance"),
                                   .checkpointJson = QStringLiteral("{}")})
                          .has_value());
    }
    CHECK(scheduler
              .ensure({.jobId = "bounded-overflow",
                       .parentJobId = std::nullopt,
                       .accountId = std::nullopt,
                       .kind = javelin::app::WorkKind::Maintenance,
                       .priority = javelin::app::WorkPriority::Maintenance,
                       .title = QStringLiteral("Overflow"),
                       .checkpointJson = QStringLiteral("{}")})
              .has_value());
}

TEST_CASE("work scheduler summary exposes progress and blocked work", "[app][work-scheduler][ui]")
{
    if (QCoreApplication::instance() == nullptr)
    {
        static int argc = 1;
        static char name[] = "work-scheduler-summary-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = QStringLiteral("work-scheduler-summary-test"),
        .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    javelin::app::WorkScheduler scheduler{connection, nullptr, std::chrono::milliseconds{0}};

    REQUIRE_FALSE(scheduler
                      .ensure({.jobId = "archive-download",
                               .parentJobId = std::nullopt,
                               .accountId = "account-1",
                               .kind = javelin::app::WorkKind::FullMailSync,
                               .priority = javelin::app::WorkPriority::Bulk,
                               .title = QStringLiteral("Download all mail in Archive"),
                               .checkpointJson = QStringLiteral("{}")})
                      .has_value());
    REQUIRE_FALSE(scheduler
                      .update("archive-download", javelin::app::WorkStatus::Running,
                              {.completedUnits = 125,
                               .totalUnits = 1000,
                               .completedBytes = 0,
                               .totalBytes = std::nullopt,
                               .detail = QStringLiteral("Reading mailbox contents")})
                      .has_value());
    CHECK(scheduler.summary().contains(QStringLiteral("Download all mail in Archive")));
    CHECK(scheduler.summary().contains(QStringLiteral("125 / 1000")));

    REQUIRE_FALSE(scheduler
                      .update("archive-download", javelin::app::WorkStatus::WaitingForNetwork,
                              {.completedUnits = 0,
                               .totalUnits = std::nullopt,
                               .completedBytes = 0,
                               .totalBytes = std::nullopt,
                               .detail = QStringLiteral("Waiting for network")})
                      .has_value());
    CHECK(scheduler.summary() == QStringLiteral("1 background task waiting"));

    REQUIRE_FALSE(scheduler
                      .update("archive-download", javelin::app::WorkStatus::Failed,
                              {.completedUnits = 0,
                               .totalUnits = std::nullopt,
                               .completedBytes = 0,
                               .totalBytes = std::nullopt,
                               .detail = QStringLiteral("")},
                              QStringLiteral("{}"), QStringLiteral("network failed"))
                      .has_value());
    CHECK(scheduler.summary() == QStringLiteral("1 background task failed"));
}
