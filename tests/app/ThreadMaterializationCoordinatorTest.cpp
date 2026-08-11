#include "app/ThreadMaterializationCoordinator.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/ThreadRepository.h"

#include <QCoreApplication>
#include <QElapsedTimer>
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
} // namespace

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

TEST_CASE("thread materialization coordinator restores incomplete durable windows",
          "[app][thread-materialization][restart]")
{
    ensureApplication();
    Fixture fixture;
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(
        threads
            .upsertMany("account-1", {javelin::jmap::domain::Thread{
                                         .id = "thread-1",
                                         .emailIds = {"email-1", "email-2", "missing-child"},
                                     }})
            .has_value());
    javelin::app::WorkScheduler scheduler{fixture.database, nullptr, std::chrono::milliseconds{0}};
    javelin::app::ThreadMaterializationCoordinator coordinator{fixture.database, scheduler};

    REQUIRE_FALSE(coordinator.restoreAccount("account-1").has_value());
    CHECK(coordinator.pendingThreadCount("account-1") == 2);
    CHECK(coordinator.isMaterializing("account-1", "thread-2"));
    CHECK(coordinator.isMaterializing("account-1", "thread-1"));
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
