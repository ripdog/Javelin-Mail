#include "jmap/cache/QueryService.h"
#include "FixtureReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/RawMessageSourceRepository.h"
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
    auto bodyMatch = subjectMatch;
    bodyMatch.id = "body-match";
    bodyMatch.threadId = "body-thread";
    bodyMatch.subject = "Ordinary update";

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

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto result = queryService.searchCachedMessageText("account-1", "rare albatross", 10);

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(result));
    const auto& items = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(result);
    REQUIRE(items.size() == 2);
    CHECK(std::ranges::any_of(items,
                              [](const auto& item) { return item.emailId == "subject-match"; }));
    CHECK(
        std::ranges::any_of(items, [](const auto& item) { return item.emailId == "body-match"; }));
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

    const auto mailboxThread =
        queryService.listMailboxThreadMessages("account-1", "mbx-inbox", "thr-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(mailboxThread));
    const auto& members =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(mailboxThread);
    REQUIRE(members.size() == 1);
    CHECK(members.front().emailId == "eml-inbox");
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
}
