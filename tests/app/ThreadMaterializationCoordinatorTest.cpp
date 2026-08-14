#include "app/ThreadMaterializationCoordinator.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/ThreadRepository.h"

#include <QCoroFuture>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPromise>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>

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
        static char name[] = "thread-materialization-coordinator-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }

    template <typename Predicate> void waitFor(Predicate predicate)
    {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < 2000)
        {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        REQUIRE(predicate());
    }

    struct Fixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;

        Fixture()
        {
            REQUIRE(directory.isValid());
            auto opened = javelin::jmap::cache::DatabaseConnection::open({
                .connectionName = QStringLiteral("thread-materialization-coordinator-test"),
                .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
            });
            REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
            database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

            QSqlQuery account{database.database()};
            account.prepare(QStringLiteral(
                "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                "VALUES('account-1','alice@example.test','https://example.test/jmap',1)"));
            REQUIRE(account.exec());

            std::vector<javelin::jmap::domain::Email> emails;
            for (const auto& [emailId, threadId] : std::vector<std::pair<std::string, std::string>>{
                     {"email-1", "thread-1"},
                     {"email-2", "thread-1"},
                     {"email-3", "thread-2"},
                 })
            {
                emails.push_back({
                    .id = emailId,
                    .blobId = std::string{"blob-"} + emailId,
                    .threadId = threadId,
                    .mailboxIds = {"inbox"},
                    .keywords = {},
                    .size = 100,
                    .receivedAt = "2026-08-11T00:00:00Z",
                    .sentAt = "2026-08-11T00:00:00Z",
                    .messageId = {},
                    .inReplyTo = {},
                    .references = {},
                    .hasAttachment = false,
                    .subject = emailId,
                    .from = {},
                    .to = {},
                    .cc = {},
                    .bcc = {},
                    .replyTo = {},
                    .preview = std::nullopt,
                });
            }
            javelin::jmap::cache::EmailRepository emailRepository{database};
            REQUIRE_FALSE(emailRepository.replaceAll("account-1", emails).has_value());

            javelin::jmap::cache::MailboxWindowRepository mailboxWindows{database};
            REQUIRE_FALSE(mailboxWindows
                              .replace({
                                  .accountId = "account-1",
                                  .mailboxId = "inbox",
                                  .queryKey = "mailbox-query",
                                  .requestedOffset = 0,
                                  .requestedLimit = 100,
                                  .position = 0,
                                  .returnedLimit = 2,
                                  .total = 2,
                                  .queryState = "query-state",
                                  .emailIds = {"email-1", "email-3"},
                              })
                              .has_value());
            javelin::jmap::cache::SearchWindowRepository searchWindows{database};
            REQUIRE_FALSE(searchWindows
                              .replace({
                                  .accountId = "account-1",
                                  .queryKey = "search-query",
                                  .offset = 0,
                                  .limit = 100,
                                  .position = 0,
                                  .returnedLimit = 2,
                                  .total = 2,
                                  .queryState = "search-state",
                                  .emailIds = {"email-2", "email-3"},
                              })
                              .has_value());
        }
    };

    class RecordingWorker final : public javelin::app::ThreadMaterializationWorker
    {
      public:
        QCoro::Task<javelin::app::ThreadMaterializationResult>
        materialize(javelin::app::ThreadMaterializationTarget target) override
        {
            targets.push_back(target);
            co_return javelin::app::ThreadMaterializationSummary{
                .threadIds = std::move(target.threadIds),
                .missingEmailIds = {},
                .completedThreadCount = 0,
                .completedEmailCount = 0,
            };
        }

        std::vector<javelin::app::ThreadMaterializationTarget> targets;
    };

    class CompletingWorker final : public javelin::app::ThreadMaterializationWorker
    {
      public:
        explicit CompletingWorker(javelin::jmap::cache::DatabaseConnection& database)
            : m_database(database)
        {
        }

        QCoro::Task<javelin::app::ThreadMaterializationResult>
        materialize(javelin::app::ThreadMaterializationTarget target) override
        {
            targets.push_back(target);
            std::vector<javelin::jmap::domain::Thread> threads;
            for (const auto& threadId : target.threadIds)
            {
                threads.push_back({
                    .id = threadId,
                    .emailIds = threadId == "thread-1"
                                    ? std::vector<std::string>{"email-1", "email-2"}
                                    : std::vector<std::string>{"email-3"},
                });
            }
            javelin::jmap::cache::ThreadRepository repository{m_database};
            if (const auto error = repository.upsertMany(target.accountId, threads))
                co_return javelin::jmap::operationError(*error);
            co_return javelin::app::ThreadMaterializationSummary{
                .threadIds = std::move(target.threadIds),
                .missingEmailIds = {},
                .completedThreadCount = threads.size(),
                .completedEmailCount = 0,
            };
        }

        std::vector<javelin::app::ThreadMaterializationTarget> targets;

      private:
        javelin::jmap::cache::DatabaseConnection& m_database;
    };

    class FailingWorker final : public javelin::app::ThreadMaterializationWorker
    {
      public:
        QCoro::Task<javelin::app::ThreadMaterializationResult>
        materialize(javelin::app::ThreadMaterializationTarget) override
        {
            co_return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
                .message = QStringLiteral("Network unavailable."),
            };
        }
    };

    class DeferredWorker final : public javelin::app::ThreadMaterializationWorker
    {
      public:
        QCoro::Task<javelin::app::ThreadMaterializationResult>
        materialize(javelin::app::ThreadMaterializationTarget target) override
        {
            targets.push_back(target);
            if (targets.size() == 1)
            {
                first.start();
                co_return co_await first.future();
            }
            co_return javelin::app::ThreadMaterializationSummary{
                .threadIds = std::move(target.threadIds),
                .missingEmailIds = {},
                .completedThreadCount = 0,
                .completedEmailCount = 0,
            };
        }

        void finishFirst(javelin::app::ThreadMaterializationResult result)
        {
            first.addResult(std::move(result));
            first.finish();
        }

        std::vector<javelin::app::ThreadMaterializationTarget> targets;
        QPromise<javelin::app::ThreadMaterializationResult> first;
    };
} // namespace

TEST_CASE("authoritative Thread wait resumes only after complete cache coverage",
          "[app][thread-materialization][selection]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    CompletingWorker worker{fixture.database};
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    const auto result =
        QCoro::waitFor(coordinator.waitForThreads("account-1", {"thread-2", "thread-1", "thread-1"},
                                                  javelin::app::WorkPriority::Interactive));

    const auto* summary = std::get_if<javelin::app::ThreadMaterializationSummary>(&result);
    REQUIRE(summary != nullptr);
    CHECK(summary->threadIds == std::vector<std::string>{"thread-1", "thread-2"});
    REQUIRE(worker.targets.size() == 1);
    CHECK(worker.targets.front().priority == javelin::app::WorkPriority::Interactive);
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    const auto threadOne = threads.coverage("account-1", "thread-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadCoverage>>(threadOne));
    const auto& coverage = std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(threadOne);
    REQUIRE(coverage.has_value());
    CHECK(coverage->childEmailsComplete);
}

TEST_CASE("authoritative Thread wait returns materialization failure without partial resolution",
          "[app][thread-materialization][selection]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    FailingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    const auto result = QCoro::waitFor(coordinator.waitForThreads("account-1", {"thread-1"}));

    const auto* error = std::get_if<javelin::jmap::OperationError>(&result);
    REQUIRE(error != nullptr);
    CHECK(error->code == javelin::jmap::OperationErrorCode::NetworkUnavailable);
}

TEST_CASE("cached Email outside current membership is recovered as incomplete Thread coverage",
          "[app][thread-materialization][recovery]")
{
    ensureApplication();
    Fixture fixture;
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1", {{.id = "thread-1", .emailIds = {"email-1"}},
                                                {.id = "thread-2", .emailIds = {"email-3"}}})
                      .has_value());
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    CompletingWorker worker{fixture.database};
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    REQUIRE_FALSE(coordinator.restoreAccount("account-1").has_value());
    waitFor([&] { return worker.targets.size() == 1; });

    CHECK(worker.targets.front().threadIds == std::vector<std::string>{"thread-1"});
    const auto coverage = threads.coverage("account-1", "thread-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage));
    const auto& repaired = std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage);
    REQUIRE(repaired.has_value());
    CHECK(repaired->childEmailsComplete);
}

TEST_CASE("thread materialization coordinator coalesces mailbox and search targets",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    RecordingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};
    std::size_t started = 0;
    std::size_t finished = 0;
    bool successful = false;
    QObject::connect(&coordinator,
                     &javelin::app::ThreadMaterializationCoordinator::materializationStarted,
                     &coordinator, [&started](const QString&, const QStringList&) { ++started; });
    QObject::connect(&coordinator,
                     &javelin::app::ThreadMaterializationCoordinator::materializationFinished,
                     &coordinator,
                     [&finished, &successful](const QString&, const QStringList&, const bool result,
                                              const QString&)
                     {
                         ++finished;
                         successful = result;
                     });

    REQUIRE_FALSE(
        coordinator.enqueueMailboxWindow("account-1", "mailbox-query", 0, 100).has_value());
    REQUIRE_FALSE(coordinator.enqueueSearchWindow("account-1", "search-query", 0, 100).has_value());
    CHECK(coordinator.pendingThreadCount("account-1") == 2);

    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets.front().threadIds == std::vector<std::string>{"thread-1", "thread-2"});
    waitFor([&] { return scheduler.activeAdmissions() == 0; });
    CHECK(started == 1);
    CHECK(finished == 1);
    CHECK(successful);

    QSqlQuery jobs{fixture.database.database()};
    REQUIRE(jobs.exec(QStringLiteral("SELECT COUNT(*) FROM background_jobs")));
    REQUIRE(jobs.next());
    CHECK(jobs.value(0).toInt() == 0);
}

TEST_CASE("observed mailbox retained windows start visible Thread materialization promptly",
          "[app][thread-materialization][priority][retained]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::seconds{5}};
    RecordingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    CHECK_FALSE(scheduler.mayStartBackgroundNetwork());
    REQUIRE_FALSE(coordinator
                      .enqueueRetainedMailbox("account-1", "inbox",
                                              javelin::app::WorkPriority::VisibleMaterialization)
                      .has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets.front().priority == javelin::app::WorkPriority::VisibleMaterialization);
    CHECK(worker.targets.front().threadIds == std::vector<std::string>{"thread-1", "thread-2"});
}

TEST_CASE("thread materialization coordinator restores incomplete durable windows",
          "[app][thread-materialization][restart]")
{
    ensureApplication();
    Fixture fixture;
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1", {javelin::jmap::domain::Thread{
                                                   .id = "thread-1",
                                                   .emailIds = {"email-1", "email-2"},
                                               }})
                      .has_value());
    QSqlQuery refresh{fixture.database.database()};
    REQUIRE(refresh.exec(
        QStringLiteral("INSERT INTO email_summary_refresh_requests(account_id,email_id) "
                       "VALUES('account-1','email-2')")));
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler};

    REQUIRE_FALSE(coordinator.restoreAccount("account-1").has_value());
    CHECK(coordinator.pendingThreadCount("account-1") == 2);
    CHECK(coordinator.isMaterializing("account-1", "thread-2"));
    CHECK(coordinator.isMaterializing("account-1", "thread-1"));
}

TEST_CASE("retained Thread recovery after cache maintenance runs as visible work",
          "[app][thread-materialization][restart][priority]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::seconds{5}};
    RecordingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    CHECK_FALSE(scheduler.mayStartBackgroundNetwork());
    REQUIRE_FALSE(
        coordinator.restoreAccount("account-1", javelin::app::WorkPriority::VisibleMaterialization)
            .has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets.front().priority == javelin::app::WorkPriority::VisibleMaterialization);
    CHECK(worker.targets.front().threadIds == std::vector<std::string>{"thread-1", "thread-2"});
}

TEST_CASE("interactive Thread demand raises queued prefetch through foreground work",
          "[app][thread-materialization][priority]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    RecordingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    scheduler.beginForegroundWork();
    REQUIRE_FALSE(coordinator
                      .enqueueRepresentativeEmails("account-1", {"email-1"},
                                                   javelin::app::WorkPriority::Freshness)
                      .has_value());
    QCoreApplication::processEvents();
    CHECK(worker.targets.empty());

    REQUIRE_FALSE(coordinator.ensureThreads("account-1", {"thread-1"}).has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets.front().priority == javelin::app::WorkPriority::Interactive);
    CHECK(worker.targets.front().threadIds == std::vector<std::string>{"thread-1"});
    scheduler.endForegroundWork();
}

TEST_CASE("interactive Thread demand does not elevate unrelated queued prefetch",
          "[app][thread-materialization][priority]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    RecordingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    scheduler.beginForegroundWork();
    REQUIRE_FALSE(coordinator
                      .enqueueRepresentativeEmails("account-1", {"email-1", "email-3"},
                                                   javelin::app::WorkPriority::Freshness)
                      .has_value());
    QCoreApplication::processEvents();
    CHECK(worker.targets.empty());

    REQUIRE_FALSE(coordinator.ensureThreads("account-1", {"thread-2"}).has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets[0].priority == javelin::app::WorkPriority::Interactive);
    CHECK(worker.targets[0].threadIds == std::vector<std::string>{"thread-2"});
    CHECK(coordinator.pendingThreadCount("account-1") == 1);

    scheduler.endForegroundWork();
    waitFor([&] { return worker.targets.size() == 2; });
    CHECK(worker.targets[1].priority == javelin::app::WorkPriority::Freshness);
    CHECK(worker.targets[1].threadIds == std::vector<std::string>{"thread-1"});
}

TEST_CASE("interactive demand on active prefetch is retained for retry after failure",
          "[app][thread-materialization][priority][recovery]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    DeferredWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    REQUIRE_FALSE(coordinator
                      .enqueueRepresentativeEmails("account-1", {"email-1"},
                                                   javelin::app::WorkPriority::Freshness)
                      .has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets[0].priority == javelin::app::WorkPriority::Freshness);

    REQUIRE_FALSE(coordinator.ensureThreads("account-1", {"thread-1"}).has_value());
    CHECK(coordinator.pendingThreadCount("account-1") == 1);
    worker.finishFirst(javelin::jmap::OperationError{
        .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
        .message = QStringLiteral("Network unavailable."),
    });

    waitFor([&] { return worker.targets.size() == 2; });
    CHECK(worker.targets[1].priority == javelin::app::WorkPriority::Interactive);
    CHECK(worker.targets[1].threadIds == std::vector<std::string>{"thread-1"});
}

TEST_CASE("completed waiter is not poisoned by unrelated batch failure",
          "[app][thread-materialization][selection][recovery]")
{
    ensureApplication();
    Fixture fixture;
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    DeferredWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    REQUIRE_FALSE(coordinator
                      .enqueueRepresentativeEmails("account-1", {"email-1", "email-3"},
                                                   javelin::app::WorkPriority::Freshness)
                      .has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets[0].threadIds == std::vector<std::string>{"thread-1", "thread-2"});

    std::optional<javelin::app::ThreadMaterializationResult> waiterResult;
    auto waitTask = coordinator.waitForThreads("account-1", {"thread-1"});
    QCoro::connect(std::move(waitTask), &coordinator,
                   [&waiterResult](javelin::app::ThreadMaterializationResult result)
                   { waiterResult = std::move(result); });
    QCoreApplication::processEvents();

    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(
        threads.upsertMany("account-1", {{.id = "thread-1", .emailIds = {"email-1", "email-2"}}})
            .has_value());
    worker.finishFirst(javelin::jmap::OperationError{
        .code = javelin::jmap::OperationErrorCode::NetworkUnavailable,
        .message = QStringLiteral("Thread 2 failed."),
    });

    waitFor([&] { return waiterResult.has_value(); });
    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(*waiterResult));
    CHECK(std::get<javelin::app::ThreadMaterializationSummary>(*waiterResult).threadIds ==
          std::vector<std::string>{"thread-1"});
    CHECK(worker.targets.size() == 1);
}

TEST_CASE("representative prefetch notices pending child summary refresh",
          "[app][thread-materialization][recovery]")
{
    ensureApplication();
    Fixture fixture;
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(
        threads.upsertMany("account-1", {{.id = "thread-1", .emailIds = {"email-1", "email-2"}}})
            .has_value());
    QSqlQuery refresh{fixture.database.database()};
    REQUIRE(refresh.exec(
        QStringLiteral("INSERT INTO email_summary_refresh_requests(account_id,email_id) "
                       "VALUES('account-1','email-2')")));
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    RecordingWorker worker;
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler,
                                                               &worker};

    REQUIRE_FALSE(coordinator.enqueueRepresentativeEmails("account-1", {"email-1"}).has_value());
    waitFor([&] { return worker.targets.size() == 1; });
    CHECK(worker.targets.front().threadIds == std::vector<std::string>{"thread-1"});
}
