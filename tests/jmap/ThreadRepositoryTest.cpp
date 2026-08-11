#include "jmap/cache/ThreadRepository.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <variant>

namespace
{

    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() != nullptr)
            {
                return;
            }

            static int argc = 1;
            static char appName[] = "javelin-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-thread-%1").arg(counter);
    }

    struct TestDatabaseContext
    {
        QTemporaryDir temporaryDir;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] TestDatabaseContext makeDatabaseContext()
    {
        TestDatabaseContext context;
        REQUIRE(context.temporaryDir.isValid());

        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = makeConnectionName(),
            .databasePath = context.temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            FAIL(error->message.toStdString());
        }

        context.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
        return context;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
            "VALUES (:account_id, :email_address, :session_url, :is_primary)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        query.bindValue(QStringLiteral(":email_address"), QStringLiteral("alice@example.com"));
        query.bindValue(QStringLiteral(":session_url"),
                        QStringLiteral("https://mail.example.com/.well-known/jmap"));
        query.bindValue(QStringLiteral(":is_primary"), 1);
        REQUIRE(query.exec());
    }

    [[nodiscard]] std::vector<javelin::jmap::domain::Thread> makeThreads()
    {
        return {
            javelin::jmap::domain::Thread{
                .id = "th-1",
                .emailIds = {"em-1", "em-2"},
            },
            javelin::jmap::domain::Thread{
                .id = "th-2",
                .emailIds = {"em-3"},
            },
        };
    }

    void seedEmail(javelin::jmap::cache::DatabaseConnection& connection, const QString& emailId,
                   const QString& threadId, const std::optional<QString>& mailboxId = std::nullopt)
    {
        QSqlQuery email{connection.database()};
        email.prepare(QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id) "
                                     "VALUES('account-1',:email_id,:thread_id)"));
        email.bindValue(QStringLiteral(":email_id"), emailId);
        email.bindValue(QStringLiteral(":thread_id"), threadId);
        REQUIRE(email.exec());
        email.finish();

        if (!mailboxId.has_value())
            return;
        QSqlQuery membership{connection.database()};
        membership.prepare(
            QStringLiteral("INSERT INTO email_mailboxes(account_id,email_id,mailbox_id) "
                           "VALUES('account-1',:email_id,:mailbox_id)"));
        membership.bindValue(QStringLiteral(":email_id"), emailId);
        membership.bindValue(QStringLiteral(":mailbox_id"), *mailboxId);
        REQUIRE(membership.exec());
    }

} // namespace

TEST_CASE("thread repository round-trips stored threads", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::ThreadRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.replaceAll("account-1", makeThreads(), "thread-state-1").has_value());

    const auto result = repository.find("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Thread>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Thread>>(result).has_value());
    const auto& thread = *std::get<std::optional<javelin::jmap::domain::Thread>>(result);
    CHECK(thread.id == "th-1");
    REQUIRE(thread.emailIds.size() == 2);
    CHECK(thread.emailIds.front() == "em-1");
    CHECK(thread.emailIds.back() == "em-2");

    const auto membershipResult = repository.findMembership("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membershipResult));
    const auto& membership =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membershipResult);
    REQUIRE(membership.has_value());
    CHECK(membership->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Current);
    CHECK(membership->globalMemberCount == 2);
    CHECK(membership->state == std::optional<std::string>{"thread-state-1"});
}

TEST_CASE("thread repository replacement removes stale threads", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::ThreadRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.replaceAll("account-1", makeThreads()).has_value());
    REQUIRE_FALSE(repository
                      .replaceAll("account-1", {javelin::jmap::domain::Thread{
                                                   .id = "th-1",
                                                   .emailIds = {"em-1"},
                                               }})
                      .has_value());

    const auto stale = repository.find("account-1", "th-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Thread>>(stale));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Thread>>(stale).has_value());
}

TEST_CASE("thread repository distinguishes membership freshness from child coverage",
          "[jmap][cache][repository][thread-coverage]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    QSqlQuery mailbox{databaseContext.connection.database()};
    REQUIRE(mailbox.exec(QStringLiteral("INSERT INTO mailboxes(account_id,mailbox_id,name) "
                                        "VALUES('account-1','mbx-inbox','Inbox')")));

    javelin::jmap::cache::ThreadRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository
                      .upsertMany("account-1",
                                  {javelin::jmap::domain::Thread{
                                      .id = "th-1",
                                      .emailIds = {"em-1", "em-2"},
                                  }},
                                  "thread-state-1")
                      .has_value());

    const auto reverse = repository.findThreadIdByEmailId("account-1", "em-2");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(reverse));
    CHECK(std::get<std::optional<std::string>>(reverse) == std::optional<std::string>{"th-1"});

    auto missing = repository.missingEmailIds("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(missing));
    CHECK(std::get<std::vector<std::string>>(missing) == std::vector<std::string>{"em-1", "em-2"});

    auto coverage = repository.coverage("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage)
              ->globalMemberCount == 2);
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage)
              ->materializedMemberCount == 0);
    CHECK_FALSE(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage)
                    ->childEmailsComplete);
    const auto unknownMailboxCount =
        repository.countMailboxMembersIfComplete("account-1", "mbx-inbox", "th-1");
    REQUIRE(std::holds_alternative<std::optional<std::size_t>>(unknownMailboxCount));
    CHECK_FALSE(std::get<std::optional<std::size_t>>(unknownMailboxCount).has_value());

    seedEmail(databaseContext.connection, QStringLiteral("em-1"), QStringLiteral("th-1"),
              QStringLiteral("mbx-inbox"));
    seedEmail(databaseContext.connection, QStringLiteral("em-2"), QStringLiteral("th-1"));

    missing = repository.missingEmailIds("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(missing));
    CHECK(std::get<std::vector<std::string>>(missing).empty());
    coverage = repository.coverage("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage)
              ->childEmailsComplete);
    const auto mailboxCount =
        repository.countMailboxMembersIfComplete("account-1", "mbx-inbox", "th-1");
    REQUIRE(std::holds_alternative<std::optional<std::size_t>>(mailboxCount));
    CHECK(std::get<std::optional<std::size_t>>(mailboxCount) == std::optional<std::size_t>{1});

    REQUIRE_FALSE(repository.markStale("account-1", std::vector<std::string>{"th-1"}).has_value());
    const auto staleMembership = repository.findMembership("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        staleMembership));
    const auto& staleRecord =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(staleMembership);
    REQUIRE(staleRecord.has_value());
    CHECK(staleRecord->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Stale);
    CHECK(staleRecord->thread.emailIds == std::vector<std::string>{"em-1", "em-2"});
    const auto staleMailboxCount =
        repository.countMailboxMembersIfComplete("account-1", "mbx-inbox", "th-1");
    REQUIRE(std::holds_alternative<std::optional<std::size_t>>(staleMailboxCount));
    CHECK_FALSE(std::get<std::optional<std::size_t>>(staleMailboxCount).has_value());
}

TEST_CASE("thread membership replacement rolls back atomically on invalid members",
          "[jmap][cache][repository][thread-coverage]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::ThreadRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository
                      .upsertMany("account-1",
                                  {javelin::jmap::domain::Thread{
                                      .id = "th-1",
                                      .emailIds = {"em-1", "em-2"},
                                  }},
                                  "thread-state-1")
                      .has_value());

    const auto error = repository.upsertMany(
        "account-1",
        {javelin::jmap::domain::Thread{.id = "th-1", .emailIds = {"duplicate", "duplicate"}}},
        "thread-state-2");
    REQUIRE(error.has_value());

    const auto result = repository.findMembership("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        result));
    const auto& membership =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(result);
    REQUIRE(membership.has_value());
    CHECK(membership->thread.emailIds == std::vector<std::string>{"em-1", "em-2"});
    CHECK(membership->state == std::optional<std::string>{"thread-state-1"});
    CHECK(membership->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Current);
}
