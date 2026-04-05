#include "jmap/cache/QueryService.h"
#include "FixtureReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/MailboxRepository.h"
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
            .databasePath = context.temporaryDir.filePath("cache.sqlite3"),
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
        query.prepare("INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
                      "VALUES (:account_id, :email_address, :session_url, :is_primary)");
        query.bindValue(":account_id", "account-1");
        query.bindValue(":email_address", "alice@example.com");
        query.bindValue(":session_url", "https://mail.example.com/.well-known/jmap");
        query.bindValue(":is_primary", 1);
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
    first.keywords = {"$flagged"};
    auto second = first;
    second.id = "eml-2";
    second.threadId = "thr-1";
    second.receivedAt = "2026-04-06T11:22:33Z";
    second.subject = "Later message";
    second.keywords = {"$seen"};
    auto third = first;
    third.id = "eml-3";
    third.threadId = "thr-2";
    third.receivedAt = "2026-04-04T11:22:33Z";
    third.subject = "Other thread";
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
}

TEST_CASE("query service SQL plans use the intended cache indexes", "[jmap][cache][query]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();

    const auto mailboxPlan = explainQueryPlan(
        databaseContext.connection.database(),
        "SELECT m.mailbox_id, m.name, m.parent_mailbox_id, m.role, m.sort_order, "
        "m.total_emails, m.unread_emails, m.total_threads, m.unread_threads, "
        "m.is_subscribed, "
        "EXISTS("
        "  SELECT 1 FROM mailboxes child "
        "  WHERE child.account_id = m.account_id AND child.parent_mailbox_id = m.mailbox_id"
        ") AS has_children "
        "FROM mailboxes m "
        "WHERE m.account_id = :account_id "
        "ORDER BY COALESCE(m.parent_mailbox_id, ''), m.sort_order, m.mailbox_id",
        {{":account_id", "account-1"}});
    CHECK(std::any_of(mailboxPlan.cbegin(), mailboxPlan.cend(), [](const QString& detail)
                      { return detail.contains("idx_mailboxes_parent", Qt::CaseInsensitive); }));

    const auto messagePlan =
        explainQueryPlan(databaseContext.connection.database(),
                         "WITH mailbox_threads AS ("
                         "  SELECT DISTINCT e.thread_id "
                         "  FROM emails e "
                         "  INNER JOIN email_mailboxes em ON em.account_id = e.account_id AND "
                         "em.email_id = e.email_id "
                         "  WHERE e.account_id = :account_id AND em.mailbox_id = :mailbox_id"
                         "), ranked_threads AS ("
                         "  SELECT e.email_id, e.thread_id, "
                         "         ROW_NUMBER() OVER (PARTITION BY e.thread_id ORDER BY "
                         "e.received_at DESC, e.email_id DESC) AS thread_rank "
                         "  FROM emails e "
                         "  INNER JOIN mailbox_threads mt ON mt.thread_id = e.thread_id "
                         "  WHERE e.account_id = :account_id"
                         ") "
                         "SELECT rt.email_id, rt.thread_id "
                         "FROM ranked_threads rt "
                         "WHERE rt.thread_rank = 1 "
                         "ORDER BY rt.email_id DESC "
                         "LIMIT :limit OFFSET :offset",
                         {{":account_id", "account-1"},
                          {":mailbox_id", "mbx-inbox"},
                          {":limit", 50},
                          {":offset", 0}});
    CHECK(std::any_of(messagePlan.cbegin(), messagePlan.cend(),
                      [](const QString& detail)
                      {
                          return detail.contains("idx_email_mailboxes_mailbox",
                                                 Qt::CaseInsensitive) ||
                                 detail.contains("idx_emails_thread", Qt::CaseInsensitive);
                      }));
}
