#include "jmap/cache/MailboxRepository.h"
#include "FixtureReader.h"
#include "jmap/domain/MailEntityParsers.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

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
        return QStringLiteral("javelin-mailbox-%1").arg(counter);
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

    [[nodiscard]] std::vector<javelin::jmap::domain::Mailbox> makeMailboxSet()
    {
        auto inbox = loadMailboxFixture();
        javelin::jmap::domain::Mailbox child{
            .id = "mbx-projects",
            .name = "Projects",
            .parentId = inbox.id,
            .role = std::nullopt,
            .sortOrder = 20,
            .totalEmails = 14,
            .unreadEmails = 2,
            .totalThreads = 10,
            .unreadThreads = 1,
            .isSubscribed = true,
            .myRights =
                {
                    .mayReadItems = true,
                    .mayAddItems = true,
                    .mayRemoveItems = true,
                    .maySetSeen = true,
                    .maySetKeywords = true,
                    .mayCreateChild = true,
                    .mayRename = true,
                    .mayDelete = false,
                    .maySubmit = true,
                },
        };

        return {inbox, child};
    }

} // namespace

TEST_CASE("mailbox repository replaces account mailboxes and reads root nodes",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MailboxRepository repository{databaseContext.connection};
    const auto mailboxes = makeMailboxSet();

    REQUIRE_FALSE(repository.replaceAll("account-1", mailboxes).has_value());

    const auto result = repository.listByParent("account-1", std::nullopt);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Mailbox>>(result));
    const auto& roots = std::get<std::vector<javelin::jmap::domain::Mailbox>>(result);
    REQUIRE(roots.size() == 1);
    CHECK(roots.front().id == "mbx-inbox");
    CHECK(roots.front().isSubscribed);
    CHECK(roots.front().myRights.maySubmit);
}

TEST_CASE("mailbox repository projects subscription state inside a transaction",
          "[jmap][cache][repository][mailbox]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::MailboxRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository.replaceAll("account-1", makeMailboxSet()).has_value());

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Hide mailbox"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    REQUIRE_FALSE(
        repository.setSubscribed(transaction, "account-1", "mbx-inbox", false).has_value());
    REQUIRE_FALSE(transaction.commit().has_value());

    const auto found = repository.find("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Mailbox>>(found));
    const auto& mailbox = std::get<std::optional<javelin::jmap::domain::Mailbox>>(found);
    REQUIRE(mailbox.has_value());
    CHECK_FALSE(mailbox->isSubscribed);
    CHECK(mailbox->name == "Inbox");
}

TEST_CASE("mailbox repository lists child mailboxes by parent id", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MailboxRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository.replaceAll("account-1", makeMailboxSet()).has_value());

    const auto result = repository.listByParent("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Mailbox>>(result));
    const auto& children = std::get<std::vector<javelin::jmap::domain::Mailbox>>(result);
    REQUIRE(children.size() == 1);
    CHECK(children.front().id == "mbx-projects");
    REQUIRE(children.front().parentId.has_value());
    CHECK(*children.front().parentId == "mbx-inbox");
    CHECK(children.front().myRights.mayCreateChild);
}

TEST_CASE("mailbox repository replacement removes stale rows", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MailboxRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository.replaceAll("account-1", makeMailboxSet()).has_value());

    auto inboxOnly = makeMailboxSet();
    inboxOnly.resize(1);
    REQUIRE_FALSE(repository.replaceAll("account-1", inboxOnly).has_value());

    const auto children = repository.listByParent("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Mailbox>>(children));
    CHECK(std::get<std::vector<javelin::jmap::domain::Mailbox>>(children).empty());
}
