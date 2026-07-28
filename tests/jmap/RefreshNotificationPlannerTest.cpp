#include "jmap/sync/RefreshNotificationPlanner.h"
#include "FixtureReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/sync/MailboxRefreshExecutor.h"

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
        return QStringLiteral("javelin-notification-planner-%1").arg(counter);
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

    [[nodiscard]] javelin::jmap::domain::Email loadEmailFixture()
    {
        const auto parsed = javelin::jmap::domain::parseEmail(
            javelin::tests::loadFixture("jmap/entities/email.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

} // namespace

TEST_CASE("refresh notification planner returns inserted unread mailbox emails",
          "[jmap][sync][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto unreadInserted = loadEmailFixture();
    unreadInserted.id = "eml-new";
    unreadInserted.threadId = "thr-new";
    unreadInserted.mailboxIds = {"mbx-inbox"};
    unreadInserted.keywords = {};
    unreadInserted.subject = "Unread new message";

    auto seenInserted = unreadInserted;
    seenInserted.id = "eml-seen";
    seenInserted.threadId = "thr-seen";
    seenInserted.keywords = {"$seen"};
    seenInserted.subject = "Seen message";

    auto otherMailbox = unreadInserted;
    otherMailbox.id = "eml-other";
    otherMailbox.threadId = "thr-other";
    otherMailbox.mailboxIds = {"mbx-archive"};
    otherMailbox.subject = "Wrong mailbox";

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(
        emailRepository.replaceAll("account-1", {unreadInserted, seenInserted, otherMailbox})
            .has_value());

    const javelin::jmap::sync::RefreshNotificationPlanner planner{databaseContext.connection};
    const auto result =
        planner.plan("account-1", "mbx-inbox",
                     javelin::jmap::sync::MailboxRefreshSummary{
                         .representativeCount = 3,
                         .usedIncrementalRefresh = false,
                         .changedEmailIds = {},
                         .insertedEmailIds = {"eml-new", "eml-seen", "eml-other", "eml-missing"},
                         .removedEmailIds = {},
                         .requiresNotificationScan = true,
                         .notificationCandidates = {},
                     });

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
        result));
    const auto& candidates =
        std::get<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(result);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().emailId == "eml-new");
    CHECK(candidates.front().threadId == "thr-new");
    CHECK(candidates.front().subject == std::optional<std::string>{"Unread new message"});
}

TEST_CASE("refresh notification planner returns empty candidates when nothing was inserted",
          "[jmap][sync][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    const javelin::jmap::sync::RefreshNotificationPlanner planner{databaseContext.connection};
    const auto result = planner.plan("account-1", "mbx-inbox",
                                     javelin::jmap::sync::MailboxRefreshSummary{
                                         .representativeCount = 0,
                                         .usedIncrementalRefresh = true,
                                         .changedEmailIds = {"eml-1"},
                                         .insertedEmailIds = {},
                                         .removedEmailIds = {},
                                         .requiresNotificationScan = false,
                                         .notificationCandidates = {},
                                     });

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
        result));
    CHECK(std::get<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(result).empty());
}

TEST_CASE("notification outbox persists pending mail until delivery", "[jmap][cache][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto unread = loadEmailFixture();
    unread.id = "eml-unread";
    unread.threadId = "thr-unread";
    unread.mailboxIds = {"mbx-inbox"};
    unread.keywords = {};

    auto seen = unread;
    seen.id = "eml-seen";
    seen.threadId = "thr-seen";
    seen.keywords = {"$seen"};

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {unread, seen}).has_value());

    javelin::jmap::cache::NotificationRepository notifications{databaseContext.connection};
    const auto hidden = notifications.enqueueUnreadMailboxEmails("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
        hidden));
    CHECK(std::get<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(hidden).empty());

    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true",
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 2,
                          .total = 2,
                          .queryState = "query-state-1",
                          .coverage = javelin::jmap::cache::QueryWindowCoverage::Server,
                          .emailIds = {"eml-unread", "eml-seen"},
                      })
                      .has_value());

    const auto first = notifications.enqueueUnreadMailboxEmails("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
        first));
    const auto& candidates =
        std::get<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(first);
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().emailId == "eml-unread");

    const auto pendingAgain = notifications.enqueueUnreadMailboxEmails("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
        pendingAgain));
    CHECK(std::get<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(pendingAgain)
              .size() == 1);
    REQUIRE_FALSE(
        notifications.markDelivered("account-1", "mbx-inbox", {"eml-unread"}).has_value());

    seen.keywords = {};
    REQUIRE_FALSE(emails.upsertMany("account-1", {seen}).has_value());
    const auto second = notifications.enqueueUnreadMailboxEmails("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(
        second));
    CHECK(std::get<std::vector<javelin::jmap::sync::RefreshNotificationCandidate>>(second).empty());
}
