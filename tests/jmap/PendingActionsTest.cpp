#include "jmap/sync/PendingActions.h"

#include "FixtureReader.h"
#include "jmap/domain/MailEntityParsers.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
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
        return QStringLiteral("javelin-pending-%1").arg(counter);
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

TEST_CASE("pending action repository round-trips typed email patch actions", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::sync::PendingActionRepository repository{databaseContext.connection};
    const javelin::jmap::sync::PendingActionRecord record{
        .pendingActionId = "action-1",
        .accountId = "account-1",
        .status = javelin::jmap::sync::PendingActionStatus::Pending,
        .emailPatch =
            {
                .emailId = "eml-1",
                .addMailboxIds = {"mbx-archive"},
                .removeMailboxIds = {"mbx-inbox"},
                .addKeywords = {"$seen"},
                .removeKeywords = {"$flagged"},
            },
    };

    REQUIRE_FALSE(repository.put(record).has_value());

    const auto result = repository.listForEmail("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::PendingActionRecord>>(result));
    const auto& records = std::get<std::vector<javelin::jmap::sync::PendingActionRecord>>(result);
    REQUIRE(records.size() == 1);
    CHECK(records.front().pendingActionId == "action-1");
    CHECK(records.front().status == javelin::jmap::sync::PendingActionStatus::Pending);
    CHECK(records.front().emailPatch.addMailboxIds == std::vector<std::string>{"mbx-archive"});
    CHECK(records.front().emailPatch.removeKeywords == std::vector<std::string>{"$flagged"});
}

TEST_CASE("pending email patch merge reapplies local mailbox and keyword deltas", "[jmap][sync]")
{
    auto email = loadEmailFixture();
    const std::vector pendingActions{
        javelin::jmap::sync::PendingActionRecord{
            .pendingActionId = "action-1",
            .accountId = "account-1",
            .status = javelin::jmap::sync::PendingActionStatus::Pending,
            .emailPatch =
                {
                    .emailId = "eml-1",
                    .addMailboxIds = {"mbx-projects"},
                    .removeMailboxIds = {"mbx-inbox"},
                    .addKeywords = {"$draft"},
                    .removeKeywords = {"$flagged"},
                },
        },
        javelin::jmap::sync::PendingActionRecord{
            .pendingActionId = "action-2",
            .accountId = "account-1",
            .status = javelin::jmap::sync::PendingActionStatus::InFlight,
            .emailPatch =
                {
                    .emailId = "eml-1",
                    .addMailboxIds = {"mbx-archive"},
                    .removeMailboxIds = {},
                    .addKeywords = {"$seen"},
                    .removeKeywords = {},
                },
        },
    };

    const auto merged = javelin::jmap::sync::mergePendingEmailPatch(email, pendingActions);
    CHECK(merged.mailboxIds == std::vector<std::string>{"mbx-archive", "mbx-projects"});
    CHECK(merged.keywords == std::vector<std::string>{"$draft", "$seen"});
}
