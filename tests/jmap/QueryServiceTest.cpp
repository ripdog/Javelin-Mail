#include "jmap/cache/QueryService.h"
#include "FixtureReader.h"
#include "app/MessageSelection.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailSearchIndex.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/domain/MailEntityParsers.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <catch2/catch_test_macros.hpp>

#include <QStringList>
#include <algorithm>
#include <memory>
#include <variant>
#include <vector>

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
        return QStringLiteral("javelin-query-%1").arg(counter);
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

    [[nodiscard]] javelin::jmap::domain::Mailbox loadMailboxFixture()
    {
        const auto parsed = javelin::jmap::domain::parseMailbox(
            javelin::tests::loadFixture("jmap/entities/mailbox.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

    [[nodiscard]] javelin::jmap::domain::Email loadEmailFixture()
    {
        const auto parsed = javelin::jmap::domain::parseEmail(
            javelin::tests::loadFixture("jmap/entities/email.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

    [[nodiscard]] QStringList
    explainQueryPlan(QSqlDatabase& database, const QString& statement,
                     const std::vector<std::pair<QString, QVariant>>& bindings)
    {
        QSqlQuery query{database};
        query.prepare(QStringLiteral("EXPLAIN QUERY PLAN %1").arg(statement));
        for (const auto& [name, value] : bindings)
        {
            query.bindValue(name, value);
        }

        REQUIRE(query.exec());

        QStringList details;
        while (query.next())
        {
            details.push_back(query.value(3).toString());
        }

        return details;
    }

} // namespace

TEST_CASE("query service returns mailbox tree rows shaped for a tree model", "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto inbox = loadMailboxFixture();
    auto child = inbox;
    child.id = "mbx-projects";
    child.name = "Projects";
    child.parentId = inbox.id;
    child.role.reset();
    child.sortOrder = 20;

    javelin::jmap::cache::MailboxRepository mailboxRepository{databaseContext.connection};
    REQUIRE_FALSE(mailboxRepository.replaceAll("account-1", {inbox, child}).has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto result = queryService.listMailboxTree("account-1");

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailboxTreeItem>>(result));
    const auto& items = std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(result);
    REQUIRE(items.size() == 2);
    CHECK(items.front().id == "mbx-inbox");
    CHECK(items.front().hasChildren);
    CHECK(items.front().isSubscribed);
    CHECK(items.front().myRights.mayReadItems);
    CHECK(items.front().myRights.maySubmit);
    CHECK_FALSE(items.front().myRights.mayCreateChild);
    CHECK_FALSE(items.back().hasChildren);
    REQUIRE(items.back().parentId.has_value());
    CHECK(*items.back().parentId == "mbx-inbox");
}

TEST_CASE("query service returns paged compact message list rows", "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto first = loadEmailFixture();
    first.threadId = "thr-1";
    first.subject = "Alpha thread";
    first.keywords = {"$flagged"};
    auto second = first;
    second.id = "eml-2";
    second.threadId = "thr-1";
    second.receivedAt = "2026-04-06T11:22:33Z";
    second.subject = "Beta thread";
    second.keywords = {"$seen"};
    auto third = first;
    third.id = "eml-3";
    third.threadId = "thr-2";
    third.receivedAt = "2026-04-04T11:22:33Z";
    third.subject = "Gamma thread";
    third.keywords = {"$seen"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {first, second, third}).has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto firstPage = queryService.listMailboxMessages("account-1", "mbx-inbox", 1, 0);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(firstPage));
    const auto& firstItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(firstPage);
    REQUIRE(firstItems.size() == 1);
    CHECK(firstItems.front().emailId == "eml-2");
    CHECK(firstItems.front().threadId == "thr-1");
    CHECK(firstItems.front().threadMessageCount == 2);
    CHECK(firstItems.front().isUnread);
    CHECK(firstItems.front().isFlagged);

    const auto secondPage = queryService.listMailboxMessages("account-1", "mbx-inbox", 1, 1);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(secondPage));
    const auto& secondItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(secondPage);
    REQUIRE(secondItems.size() == 1);
    CHECK(secondItems.front().emailId == "eml-3");
    CHECK(secondItems.front().threadId == "thr-2");
    CHECK(secondItems.front().threadMessageCount == 1);
    CHECK_FALSE(secondItems.front().isUnread);
    CHECK_FALSE(secondItems.front().isFlagged);
    REQUIRE(secondItems.front().from.has_value());
    CHECK(secondItems.front().from->email == "alice@example.com");

    const auto oldestFirst = queryService.listMailboxMessages(
        "account-1", "mbx-inbox", 2, 0,
        javelin::jmap::query::EmailListSort{
            .direction = javelin::jmap::query::EmailListSortDirection::Ascending,
        });
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(oldestFirst));
    const auto& oldestItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(oldestFirst);
    REQUIRE(oldestItems.size() == 2);
    CHECK(oldestItems[0].emailId == "eml-3");
    CHECK(oldestItems[1].emailId == "eml-1");

    const auto subjectDescending = queryService.listMailboxMessages(
        "account-1", "mbx-inbox", 2, 0,
        javelin::jmap::query::EmailListSort{
            .property = javelin::jmap::query::EmailListSortProperty::Subject,
            .direction = javelin::jmap::query::EmailListSortDirection::Descending,
        });
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(
        subjectDescending));
    const auto& subjectItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(subjectDescending);
    REQUIRE(subjectItems.size() == 2);
    CHECK(subjectItems[0].emailId == "eml-3");
    CHECK(subjectItems[1].emailId == "eml-2");
}

TEST_CASE("offline mailbox coverage exposes only the published crawl generation and projections",
          "[jmap][cache][query][offline]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto newest = loadEmailFixture();
    newest.id = "staged-newest";
    newest.threadId = "thread-newest";
    newest.receivedAt = "2026-07-21T03:00:00Z";
    auto staged = newest;
    staged.id = "staged-second";
    staged.threadId = "thread-second";
    staged.receivedAt = "2026-07-21T02:00:00Z";
    auto projected = newest;
    projected.id = "projected-addition";
    projected.threadId = "thread-projected";
    projected.receivedAt = "2026-07-21T01:30:00Z";
    auto stale = newest;
    stale.id = "old-window-only";
    stale.threadId = "thread-stale";
    stale.receivedAt = "2026-07-21T01:00:00Z";

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {newest, staged, projected, stale}).has_value());

    QSqlQuery setup{databaseContext.connection.database()};
    REQUIRE(setup.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,generation) "
        "VALUES('account-1','mbx-inbox',1,'enumerating',7)")));
    REQUIRE(setup.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_membership(account_id,mailbox_id,email_id,generation,"
        "position) VALUES('account-1','mbx-inbox','staged-newest',7,0),"
        "('account-1','mbx-inbox','staged-second',7,1)")));
    REQUIRE(setup.exec(QStringLiteral(
        "INSERT INTO mutation_journal(mutation_id,account_id,data_type,object_id,mutation_kind,"
        "status,payload_json) VALUES('mutation-1','account-1','Email','projected-addition',"
        "'email_patch','pending','{}')")));

    javelin::jmap::cache::QueryService queries{databaseContext.connection};
    const auto coverageResult = queries.offlineMailboxCoverage("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::OfflineMailboxCoverage>>(
        coverageResult));
    const auto& coverage =
        std::get<std::optional<javelin::jmap::cache::OfflineMailboxCoverage>>(coverageResult);
    REQUIRE(coverage.has_value());
    CHECK(coverage->generation == 7);
    CHECK(coverage->representativeCount == 3);
    CHECK_FALSE(coverage->enumerationComplete);

    const auto pageResult = queries.listOfflineMailboxMessages("account-1", "mbx-inbox", 7, 10);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(pageResult));
    const auto& page = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(pageResult);
    REQUIRE(page.size() == 3);
    CHECK(page[0].emailId == "staged-newest");
    CHECK(page[1].emailId == "staged-second");
    CHECK(page[2].emailId == "projected-addition");
    CHECK(std::ranges::none_of(page,
                               [](const auto& item) { return item.emailId == "old-window-only"; }));

    REQUIRE(setup.exec(QStringLiteral(
        "UPDATE offline_mailbox_scopes SET status='fetching' WHERE account_id='account-1' "
        "AND mailbox_id='mbx-inbox'")));
    const auto completeCoverageResult = queries.offlineMailboxCoverage("account-1", "mbx-inbox");
    const auto& completeCoverage =
        std::get<std::optional<javelin::jmap::cache::OfflineMailboxCoverage>>(
            completeCoverageResult);
    REQUIRE(completeCoverage.has_value());
    CHECK(completeCoverage->enumerationComplete);
}

TEST_CASE("query service full text search covers cached subjects and bodies",
          "[jmap][cache][query][search]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto subjectMatch = loadEmailFixture();
    subjectMatch.id = "subject-match";
    subjectMatch.threadId = "subject-thread";
    subjectMatch.subject = "A rare albatross sighting";
    subjectMatch.receivedAt = "2026-03-30T06:06:00Z";
    auto bodyMatch = subjectMatch;
    bodyMatch.id = "body-match";
    bodyMatch.threadId = "body-thread";
    bodyMatch.subject = "Ordinary update";
    bodyMatch.receivedAt = "2026-04-01T21:56:00Z";

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {subjectMatch, bodyMatch}).has_value());
    javelin::jmap::cache::RawMessageSourceRepository sourceRepository{databaseContext.connection};
    REQUIRE_FALSE(
        sourceRepository
            .upsert("account-1", {.emailId = bodyMatch.id,
                                  .blobId = bodyMatch.blobId,
                                  .payload = QByteArrayLiteral(
                                      "From: alice@example.com\r\nSubject: Ordinary update\r\n"
                                      "Content-Type: text/plain; charset=utf-8\r\n\r\n"
                                      "The telescope found a rare albatross.\r\n")})
            .has_value());
    javelin::jmap::cache::MailSearchIndex searchIndex{databaseContext.connection};
    const auto subjectIndexError =
        searchIndex.upsert("account-1", {.emailId = subjectMatch.id,
                                         .sourceHash = "subject-metadata-v1",
                                         .subject = QStringLiteral("A rare albatross sighting"),
                                         .body = {}});
    const std::string subjectIndexMessage =
        subjectIndexError ? subjectIndexError->message.toStdString() : std::string{};
    INFO(subjectIndexMessage);
    REQUIRE_FALSE(subjectIndexError.has_value());
    REQUIRE_FALSE(
        searchIndex
            .upsert("account-1", {.emailId = bodyMatch.id,
                                  .sourceHash = "body-source-v1",
                                  .subject = QStringLiteral("Ordinary update"),
                                  .body = QStringLiteral("The telescope found a rare albatross.")})
            .has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto result = queryService.searchCachedMessageText("account-1", "rare albatross", 10);

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(result));
    const auto& items = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(result);
    REQUIRE(items.size() == 2);
    CHECK(std::ranges::any_of(items,
                              [](const auto& item) { return item.emailId == "subject-match"; }));
    CHECK(
        std::ranges::any_of(items, [](const auto& item) { return item.emailId == "body-match"; }));

    const auto snapshot = queryService.searchAllCachedMessageText("account-1", "rare albatross");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(snapshot));
    const auto& snapshotItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(snapshot);
    REQUIRE(snapshotItems.size() == 2);
    CHECK(snapshotItems.at(0).emailId == "body-match");
    CHECK(snapshotItems.at(1).emailId == "subject-match");

    const auto ascendingSnapshot = queryService.searchAllCachedMessageText(
        "account-1", "rare albatross",
        {.property = javelin::jmap::query::EmailListSortProperty::ReceivedAt,
         .direction = javelin::jmap::query::EmailListSortDirection::Ascending});
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(
        ascendingSnapshot));
    const auto& ascendingItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(ascendingSnapshot);
    REQUIRE(ascendingItems.size() == 2);
    CHECK(ascendingItems.at(0).emailId == "subject-match");
    CHECK(ascendingItems.at(1).emailId == "body-match");
}

TEST_CASE("query service returns thread messages in cached thread order", "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto first = loadEmailFixture();
    first.id = "eml-1";
    first.threadId = "thr-1";
    first.receivedAt = "2026-03-30T06:06:00Z";
    first.sentAt = "2026-03-30T06:06:00Z";
    first.subject = "First";

    auto second = first;
    second.id = "eml-2";
    second.receivedAt = "2026-03-30T10:25:00Z";
    second.sentAt = "2026-03-30T10:25:00Z";
    second.subject = "Second";

    auto third = first;
    third.id = "eml-3";
    third.receivedAt = "2026-04-01T21:56:00Z";
    third.sentAt = "2026-04-01T21:56:00Z";
    third.subject = "Third";

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {first, second, third}).has_value());

    javelin::jmap::cache::ThreadRepository threadRepository{databaseContext.connection};
    REQUIRE_FALSE(threadRepository
                      .replaceAll("account-1", {javelin::jmap::domain::Thread{
                                                   .id = "thr-1",
                                                   .emailIds = {"eml-2", "eml-1", "eml-3"},
                                               }})
                      .has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto result = queryService.listThreadMessages("account-1", "thr-1");

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(result));
    const auto& items = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(result);
    REQUIRE(items.size() == 3);
    CHECK(items[0].emailId == "eml-2");
    CHECK(items[1].emailId == "eml-1");
    CHECK(items[2].emailId == "eml-3");
}

TEST_CASE("mailbox thread queries exclude members moved to another mailbox", "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto inboxEmail = loadEmailFixture();
    inboxEmail.id = "eml-inbox";
    inboxEmail.threadId = "thr-1";
    inboxEmail.receivedAt = "2026-03-30T06:06:00Z";
    inboxEmail.subject = "Inbox member";
    inboxEmail.mailboxIds = {"mbx-inbox"};

    auto archivedEmail = inboxEmail;
    archivedEmail.id = "eml-archive";
    archivedEmail.receivedAt = "2026-04-01T21:56:00Z";
    archivedEmail.subject = "Archived member";
    archivedEmail.mailboxIds = {"mbx-archive"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {inboxEmail, archivedEmail}).has_value());

    javelin::jmap::cache::ThreadRepository threadRepository{databaseContext.connection};
    REQUIRE_FALSE(threadRepository
                      .replaceAll("account-1", {javelin::jmap::domain::Thread{
                                                   .id = "thr-1",
                                                   .emailIds = {"eml-inbox", "eml-archive"},
                                               }})
                      .has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto mailboxPage = queryService.listMailboxMessages("account-1", "mbx-inbox", 100, 0);
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(mailboxPage));
    const auto& summaries =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(mailboxPage);
    REQUIRE(summaries.size() == 1);
    CHECK(summaries.front().emailId == "eml-inbox");
    CHECK(summaries.front().threadMessageCount == 1);

    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 1,
                          .total = 1,
                          .queryState = "query-state-1",
                          .emailIds = {"eml-inbox"},
                      })
                      .has_value());
    const auto cachedWindow = queryService.loadMailboxWindow("account-1", queryKey, 0, 100, {});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowPage>>(
        cachedWindow));
    const auto& cachedPage =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowPage>>(cachedWindow);
    REQUIRE(cachedPage.has_value());
    REQUIRE(cachedPage->items.size() == 1);
    CHECK(cachedPage->items.front().threadMessageCount == 1);

    const auto mailboxThread =
        queryService.listMailboxThreadMessages("account-1", "mbx-inbox", "thr-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(mailboxThread));
    const auto& members =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(mailboxThread);
    REQUIRE(members.size() == 1);
    CHECK(members.front().emailId == "eml-inbox");

    const javelin::app::MessageSelection selection{
        javelin::app::SelectedEmail{.emailId = "eml-inbox"},
        javelin::app::SelectedCollapsedThread{
            .threadId = "thr-1",
            .representativeEmailId = "eml-archive",
        },
    };
    const auto resolved =
        javelin::app::resolveMessageSelection(queryService, "account-1", "mbx-inbox", selection);
    REQUIRE(std::holds_alternative<std::vector<std::string>>(resolved));
    CHECK(std::get<std::vector<std::string>>(resolved) == std::vector<std::string>{"eml-inbox"});
}

TEST_CASE("query service rehydrates cached representative rows by email id order",
          "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto inbox = loadMailboxFixture();
    auto archive = inbox;
    archive.id = "mbx-archive";
    archive.name = "Archive";
    archive.role = "archive";
    archive.sortOrder = 20;
    javelin::jmap::cache::MailboxRepository mailboxRepository{databaseContext.connection};
    REQUIRE_FALSE(mailboxRepository.replaceAll("account-1", {inbox, archive}).has_value());

    auto first = loadEmailFixture();
    first.id = "eml-1";
    first.threadId = "thr-1";
    first.receivedAt = "2026-04-05T11:22:33Z";
    first.keywords = {"$seen"};

    auto second = first;
    second.id = "eml-2";
    second.threadId = "thr-1";
    second.receivedAt = "2026-04-06T11:22:33Z";
    second.subject = "Later message";
    second.keywords = {"$flagged"};

    auto third = first;
    third.id = "eml-3";
    third.threadId = "thr-2";
    third.receivedAt = "2026-04-04T11:22:33Z";
    third.subject = "Other thread";
    third.keywords.clear();
    third.mailboxIds = {"mbx-inbox", "mbx-archive"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {first, second, third}).has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto result = queryService.listMessagesByEmailIds("account-1", {"eml-3", "eml-2"});

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(result));
    const auto& items = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(result);
    REQUIRE(items.size() == 2);
    CHECK(items[0].emailId == "eml-3");
    CHECK(items[0].threadId == "thr-2");
    CHECK(items[0].threadMessageCount == 1);
    CHECK(items[0].isUnread);
    CHECK_FALSE(items[0].isFlagged);
    CHECK(items[0].mailboxNames == std::vector<std::string>{"Inbox", "Archive"});
    CHECK(items[1].emailId == "eml-2");
    CHECK(items[1].threadId == "thr-1");
    CHECK(items[1].threadMessageCount == 2);
    CHECK(items[1].isUnread);
    CHECK(items[1].isFlagged);

    const auto inboxUnread = queryService.countUnreadMailboxEmails("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::size_t>(inboxUnread));
    CHECK(std::get<std::size_t>(inboxUnread) == 2);
    const auto archiveUnread = queryService.countUnreadMailboxEmails("account-1", "mbx-archive");
    REQUIRE(std::holds_alternative<std::size_t>(archiveUnread));
    CHECK(std::get<std::size_t>(archiveUnread) == 1);
}

TEST_CASE("query service loads sparse mailbox pages from authoritative window order",
          "[jmap][cache][query][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    auto newer = loadEmailFixture();
    newer.id = "newer";
    newer.threadId = "thread-newer";
    newer.receivedAt = "2026-07-17T10:00:00Z";
    auto older = newer;
    older.id = "older";
    older.threadId = "thread-older";
    older.receivedAt = "2025-01-01T10:00:00Z";
    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {newer, older}).has_value());

    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 200,
                          .requestedLimit = 100,
                          .position = 200,
                          .returnedLimit = 50,
                          .total = 1000,
                          .queryState = "state-1",
                          .emailIds = {"older", "newer"},
                      })
                      .has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto result = queryService.loadMailboxWindow("account-1", queryKey, 200, 100);
    const auto* page = std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&result);
    REQUIRE(page != nullptr);
    REQUIRE(page->has_value());
    REQUIRE((*page)->items.size() == 2);
    CHECK((*page)->items[0].emailId == "older");
    CHECK((*page)->items[1].emailId == "newer");
    CHECK((*page)->position == 200);
    CHECK((*page)->returnedLimit == 50);
    CHECK((*page)->total == std::optional<std::size_t>{1000});
}

TEST_CASE("query service distinguishes a stale mailbox window from an empty page",
          "[jmap][cache][query][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 1,
                          .queryState = "state-1",
                          .emailIds = {},
                      })
                      .has_value());
    REQUIRE_FALSE(windows.invalidateMailbox("account-1", "mbx-inbox").has_value());

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto staleResult = queryService.loadMailboxWindow("account-1", queryKey, 0, 100);
    const auto* stale =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&staleResult);
    REQUIRE(stale != nullptr);
    REQUIRE(stale->has_value());
    CHECK_FALSE((*stale)->isAuthoritative);

    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 0,
                          .queryState = "state-2",
                          .emailIds = {},
                      })
                      .has_value());

    const auto emptyResult = queryService.loadMailboxWindow("account-1", queryKey, 0, 100);
    const auto* empty =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowPage>>(&emptyResult);
    REQUIRE(empty != nullptr);
    REQUIRE(empty->has_value());
    CHECK((*empty)->isAuthoritative);
    CHECK((*empty)->items.empty());
    CHECK((*empty)->total == std::optional<std::size_t>{0});
}

TEST_CASE("mailbox query changes rebase every contiguous cached page",
          "[jmap][cache][query][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    for (const auto& [offset, ids] : std::vector<std::pair<std::size_t, std::vector<std::string>>>{
             {0, {"a", "b"}}, {2, {"c", "d"}}})
    {
        REQUIRE_FALSE(windows
                          .replace({
                              .accountId = "account-1",
                              .mailboxId = "mbx-inbox",
                              .queryKey = queryKey,
                              .requestedOffset = offset,
                              .requestedLimit = 2,
                              .position = offset,
                              .returnedLimit = 2,
                              .total = 100,
                              .queryState = "state-1",
                              .emailIds = ids,
                          })
                          .has_value());
    }

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Test mailbox prefix rebase"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    REQUIRE_FALSE(windows
                      .rebaseContiguousPrefix(transaction, "account-1", "mbx-inbox", queryKey,
                                              "state-1", "state-2", {{.emailId = "x", .index = 1}},
                                              {"b"}, 100)
                      .has_value());
    REQUIRE_FALSE(transaction.commit().has_value());

    const auto firstResult = windows.find("account-1", queryKey, 0, 2);
    const auto secondResult = windows.find("account-1", queryKey, 2, 2);
    const auto& first =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(firstResult);
    const auto& second =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(secondResult);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->queryState == "state-2");
    CHECK(second->queryState == "state-2");
    CHECK(first->emailIds == std::vector<std::string>{"a", "x"});
    CHECK(second->emailIds == std::vector<std::string>{"c", "d"});
    CHECK(first->total == std::optional<std::size_t>{100});

    auto secondTransactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Test mailbox prefix removal"));
    REQUIRE(
        std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(secondTransactionResult));
    auto secondTransaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(secondTransactionResult));
    REQUIRE_FALSE(windows
                      .rebaseContiguousPrefix(secondTransaction, "account-1", "mbx-inbox", queryKey,
                                              "state-2", "state-3", {}, {"x"}, 99)
                      .has_value());
    REQUIRE_FALSE(secondTransaction.commit().has_value());

    const auto shiftedFirstResult = windows.find("account-1", queryKey, 0, 2);
    const auto shiftedSecondResult = windows.find("account-1", queryKey, 2, 2);
    const auto& shiftedFirst =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(shiftedFirstResult);
    const auto& shiftedSecond =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(shiftedSecondResult);
    REQUIRE(shiftedFirst.has_value());
    REQUIRE(shiftedSecond.has_value());
    CHECK(shiftedFirst->isAuthoritative);
    CHECK(shiftedFirst->emailIds == std::vector<std::string>{"a", "c"});
    CHECK_FALSE(shiftedSecond->isAuthoritative);
    CHECK(shiftedSecond->emailIds == std::vector<std::string>{"d"});
}

TEST_CASE("offline mailboxes retain every materialized query window",
          "[jmap][cache][query][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    QSqlQuery scope{databaseContext.connection.database()};
    REQUIRE(scope.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,generation) "
        "VALUES('account-1','mbx-inbox',1,'enumerating',1)")));
    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    for (std::size_t page = 0; page < 13; ++page)
    {
        REQUIRE_FALSE(windows
                          .replace({
                              .accountId = "account-1",
                              .mailboxId = "mbx-inbox",
                              .queryKey = queryKey,
                              .requestedOffset = page * 100,
                              .requestedLimit = 100,
                              .position = page * 100,
                              .returnedLimit = 100,
                              .total = std::nullopt,
                              .queryState = "state-1",
                              .emailIds = {"id-" + std::to_string(page)},
                          })
                          .has_value());
    }
    const auto firstResult = windows.find("account-1", queryKey, 0, 100);
    const auto& first =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(firstResult);
    REQUIRE(first.has_value());
    CHECK(first->emailIds == std::vector<std::string>{"id-0"});

    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = std::nullopt,
                          .queryState = "state-2",
                          .emailIds = {"new-id"},
                      })
                      .has_value());
    const auto deepResult = windows.find("account-1", queryKey, 1200, 100);
    const auto& deep =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(deepResult);
    REQUIRE(deep.has_value());
    CHECK_FALSE(deep->isAuthoritative);
    CHECK(deep->emailIds == std::vector<std::string>{"id-12"});
}

TEST_CASE("complete offline mailbox state versions locally sorted page windows",
          "[jmap][cache][query][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    QSqlQuery scope{databaseContext.connection.database()};
    REQUIRE(scope.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,generation) "
        "VALUES('account-1','mbx-inbox',1,'complete',1)")));

    const std::string canonicalQueryKey =
        "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::SyncStateRepository states{databaseContext.connection};
    REQUIRE_FALSE(states
                      .upsert({.accountId = "account-1",
                               .objectType = "EmailQuery",
                               .queryKey = canonicalQueryKey},
                              "state-current")
                      .has_value());

    javelin::jmap::cache::QueryService queries{databaseContext.connection};
    const auto result =
        queries.completeOfflineMailboxQueryState("account-1", "mbx-inbox", canonicalQueryKey);
    const auto* state = std::get_if<std::optional<std::string>>(&result);
    REQUIRE(state != nullptr);
    REQUIRE(state->has_value());
    CHECK(**state == "state-current");
}

TEST_CASE("same-state mailbox pages inherit authoritative query totals",
          "[jmap][cache][query][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    QSqlQuery scope{databaseContext.connection.database()};
    REQUIRE(scope.exec(QStringLiteral(
        "INSERT INTO offline_mailbox_scopes(account_id,mailbox_id,desired,status,generation) "
        "VALUES('account-1','mbx-inbox',1,'enumerating',1)")));

    const std::string queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true";
    javelin::jmap::cache::MailboxWindowRepository windows{databaseContext.connection};
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 500,
                          .queryState = "state-1",
                          .emailIds = {"id-0"},
                      })
                      .has_value());
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 100,
                          .requestedLimit = 100,
                          .position = 100,
                          .returnedLimit = 100,
                          .total = std::nullopt,
                          .queryState = "state-1",
                          .emailIds = {"id-100"},
                      })
                      .has_value());
    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 0,
                          .requestedLimit = 100,
                          .position = 0,
                          .returnedLimit = 100,
                          .total = std::nullopt,
                          .queryState = "state-1",
                          .emailIds = {"id-0-new"},
                      })
                      .has_value());

    const auto firstResult = windows.find("account-1", queryKey, 0, 100);
    const auto secondResult = windows.find("account-1", queryKey, 100, 100);
    const auto& first =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(firstResult);
    const auto& second =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(secondResult);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->total == std::optional<std::size_t>{500});
    CHECK(second->total == std::optional<std::size_t>{500});

    REQUIRE_FALSE(windows
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = queryKey,
                          .requestedOffset = 200,
                          .requestedLimit = 100,
                          .position = 200,
                          .returnedLimit = 100,
                          .total = std::nullopt,
                          .queryState = "state-2",
                          .emailIds = {"id-200"},
                      })
                      .has_value());
    const auto newStateResult = windows.find("account-1", queryKey, 200, 100);
    const auto& newState =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(newStateResult);
    REQUIRE(newState.has_value());
    CHECK_FALSE(newState->total.has_value());

    const auto staleResult = windows.find("account-1", queryKey, 0, 100);
    const auto& stale =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(staleResult);
    REQUIRE(stale.has_value());
    CHECK_FALSE(stale->isAuthoritative);
}

TEST_CASE("query service SQL plans use the intended cache indexes", "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();

    const auto mailboxPlan = explainQueryPlan(
        databaseContext.connection.database(),
        QStringLiteral(
            "SELECT m.mailbox_id, m.name, m.parent_mailbox_id, m.role, m.sort_order, "
            "m.total_emails, m.unread_emails, m.total_threads, m.unread_threads, "
            "m.is_subscribed, "
            "EXISTS("
            "  SELECT 1 FROM mailboxes child "
            "  WHERE child.account_id = m.account_id AND child.parent_mailbox_id = m.mailbox_id"
            ") AS has_children "
            "FROM mailboxes m "
            "WHERE m.account_id = :account_id "
            "ORDER BY COALESCE(m.parent_mailbox_id, ''), m.sort_order, m.mailbox_id"),
        {{QStringLiteral(":account_id"), QStringLiteral("account-1")}});
    CHECK(std::any_of(
        mailboxPlan.cbegin(), mailboxPlan.cend(), [](const QString& detail)
        { return detail.contains(QStringLiteral("idx_mailboxes_parent"), Qt::CaseInsensitive); }));

    const auto messagePlan = explainQueryPlan(
        databaseContext.connection.database(),
        QStringLiteral("WITH mailbox_email_ids AS MATERIALIZED ("
                       "  SELECT em.email_id "
                       "  FROM email_mailboxes em INDEXED BY idx_email_mailboxes_mailbox "
                       "  WHERE em.account_id = :account_id AND em.mailbox_id = :mailbox_id"
                       "), mailbox_threads AS MATERIALIZED ("
                       "  SELECT DISTINCT e.thread_id "
                       "  FROM mailbox_email_ids me "
                       "  CROSS JOIN emails e ON e.account_id = :account_id AND "
                       "e.email_id = me.email_id"
                       "), ranked_threads AS ("
                       "  SELECT e.email_id, e.thread_id, "
                       "         ROW_NUMBER() OVER (PARTITION BY e.thread_id ORDER BY "
                       "e.received_at DESC, e.email_id DESC) AS thread_rank "
                       "  FROM mailbox_threads mt "
                       "  CROSS JOIN emails e INDEXED BY idx_emails_thread "
                       "       ON e.account_id = :account_id AND e.thread_id = mt.thread_id"
                       ") "
                       "SELECT rt.email_id, rt.thread_id "
                       "FROM ranked_threads rt "
                       "WHERE rt.thread_rank = 1 "
                       "ORDER BY rt.email_id DESC "
                       "LIMIT :limit OFFSET :offset"),
        {{QStringLiteral(":account_id"), QStringLiteral("account-1")},
         {QStringLiteral(":mailbox_id"), QStringLiteral("mbx-inbox")},
         {QStringLiteral(":limit"), 50},
         {QStringLiteral(":offset"), 0}});
    CHECK(std::any_of(messagePlan.cbegin(), messagePlan.cend(),
                      [](const QString& detail)
                      {
                          return detail.contains(QStringLiteral("idx_email_mailboxes_mailbox"),
                                                 Qt::CaseInsensitive) ||
                                 detail.contains(QStringLiteral("idx_emails_thread"),
                                                 Qt::CaseInsensitive);
                      }));

    const auto windowHydrationPlan = explainQueryPlan(
        databaseContext.connection.database(),
        QStringLiteral("WITH requested AS MATERIALIZED ("
                       "  SELECT value AS email_id FROM json_each(:email_ids_json)"
                       "), requested_threads AS MATERIALIZED ("
                       "  SELECT DISTINCT e.thread_id FROM requested r "
                       "  CROSS JOIN emails e ON e.account_id=:account_id AND e.email_id=r.email_id"
                       "), thread_counts AS ("
                       "  SELECT e.thread_id, COUNT(*) FROM requested_threads rt "
                       "  CROSS JOIN emails e INDEXED BY idx_emails_thread "
                       "    ON e.account_id=:account_id AND e.thread_id=rt.thread_id "
                       "  GROUP BY e.thread_id"
                       ") SELECT r.email_id FROM requested r "
                       "CROSS JOIN emails e ON e.account_id=:account_id AND e.email_id=r.email_id "
                       "LEFT JOIN thread_counts tc ON tc.thread_id=e.thread_id"),
        {{QStringLiteral(":account_id"), QStringLiteral("account-1")},
         {QStringLiteral(":email_ids_json"), QStringLiteral("[\"eml-1\"]")}});
    CHECK(std::any_of(windowHydrationPlan.cbegin(), windowHydrationPlan.cend(),
                      [](const QString& detail)
                      {
                          return detail.contains(QStringLiteral("account_id=? AND email_id=?"),
                                                 Qt::CaseInsensitive);
                      }));
    CHECK(std::any_of(windowHydrationPlan.cbegin(), windowHydrationPlan.cend(),
                      [](const QString& detail)
                      {
                          return detail.contains(
                              QStringLiteral("idx_emails_thread (account_id=? AND thread_id=?)"),
                              Qt::CaseInsensitive);
                      }));
}
