#include "jmap/sync/MutationJournal.h"
#include "jmap/sync/EmailMutationJournal.h"

#include "FixtureReader.h"
#include "jmap/cache/EmailRepository.h"
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

TEST_CASE("mutation journal round-trips typed Email patch mutations", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::sync::EmailMutationJournal repository{databaseContext.connection};
    const javelin::jmap::sync::EmailMutationRecord record{
        .mutationId = "action-1",
        .operationGroupId = std::nullopt,
        .accountId = "account-1",
        .status = javelin::jmap::sync::MutationStatus::Pending,
        .patch =
            {
                .emailId = "eml-1",
                .addMailboxIds = {"mbx-archive"},
                .removeMailboxIds = {"mbx-inbox"},
                .addKeywords = {"$seen"},
                .removeKeywords = {"$flagged"},
                .destroy = true,
            },
        .baseMailboxIds = std::vector<std::string>{"mbx-inbox"},
        .baseKeywords = std::vector<std::string>{"$flagged"},
        .baseState = std::nullopt,
        .acceptedState = std::nullopt,
        .errorJson = std::nullopt,
    };

    REQUIRE_FALSE(repository.put(record).has_value());

    const auto result = repository.listForEmail("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(result));
    const auto& records = std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(result);
    REQUIRE(records.size() == 1);
    CHECK(records.front().mutationId == "action-1");
    CHECK(records.front().status == javelin::jmap::sync::MutationStatus::Pending);
    CHECK(records.front().patch.addMailboxIds == std::vector<std::string>{"mbx-archive"});
    CHECK(records.front().patch.removeKeywords == std::vector<std::string>{"$flagged"});
    CHECK(records.front().patch.destroy);
    CHECK(records.front().baseMailboxIds == std::vector<std::string>{"mbx-inbox"});
    CHECK(records.front().baseKeywords == std::vector<std::string>{"$flagged"});
}

TEST_CASE("pending email patch merge reapplies local mailbox and keyword deltas", "[jmap][sync]")
{
    auto email = loadEmailFixture();
    const std::vector pendingActions{
        javelin::jmap::sync::EmailMutationRecord{
            .mutationId = "action-1",
            .operationGroupId = std::nullopt,
            .accountId = "account-1",
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .patch =
                {
                    .emailId = "eml-1",
                    .addMailboxIds = {"mbx-projects"},
                    .removeMailboxIds = {"mbx-inbox"},
                    .addKeywords = {"$draft"},
                    .removeKeywords = {"$flagged"},
                },
            .baseMailboxIds = std::nullopt,
            .baseKeywords = std::nullopt,
            .baseState = std::nullopt,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        },
        javelin::jmap::sync::EmailMutationRecord{
            .mutationId = "action-2",
            .operationGroupId = std::nullopt,
            .accountId = "account-1",
            .status = javelin::jmap::sync::MutationStatus::InFlight,
            .patch =
                {
                    .emailId = "eml-1",
                    .addMailboxIds = {"mbx-archive"},
                    .removeMailboxIds = {},
                    .addKeywords = {"$seen"},
                    .removeKeywords = {},
                },
            .baseMailboxIds = std::nullopt,
            .baseKeywords = std::nullopt,
            .baseState = std::nullopt,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        },
    };

    const auto merged = javelin::jmap::sync::projectEmailMutations(email, pendingActions);
    CHECK(merged.mailboxIds == std::vector<std::string>{"mbx-archive", "mbx-projects"});
    CHECK(merged.keywords == std::vector<std::string>{"$draft", "$seen"});
}

TEST_CASE("generic mutation journal scopes lifecycle records by data type", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::sync::MutationJournalRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository
                      .put({
                          .mutationId = "contact-1",
                          .operationGroupId = "group-1",
                          .domain = {.accountId = "account-1", .dataType = "ContactCard"},
                          .objectId = "card-1",
                          .mutationKind = "contact_patch",
                          .status = javelin::jmap::sync::MutationStatus::Pending,
                          .payloadJson = R"({"name":"Alice"})",
                          .baseState = "state-1",
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());
    REQUIRE_FALSE(repository
                      .put({
                          .mutationId = "event-1",
                          .operationGroupId = std::nullopt,
                          .domain = {.accountId = "account-1", .dataType = "CalendarEvent"},
                          .objectId = "event-1",
                          .mutationKind = "calendar_patch",
                          .status = javelin::jmap::sync::MutationStatus::Pending,
                          .payloadJson = R"({"title":"Review"})",
                          .baseState = "state-2",
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());

    const auto contacts =
        repository.listByStatus({.accountId = "account-1", .dataType = "ContactCard"},
                                javelin::jmap::sync::MutationStatus::Pending, 10);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(contacts));
    const auto& records = std::get<std::vector<javelin::jmap::sync::MutationRecord>>(contacts);
    REQUIRE(records.size() == 1);
    CHECK(records.front().mutationId == "contact-1");
    CHECK(records.front().operationGroupId == std::optional<std::string>{"group-1"});
    CHECK(records.front().baseState == std::optional<std::string>{"state-1"});

    const auto grouped = repository.listForOperationGroup(
        {.accountId = "account-1", .dataType = "ContactCard"}, "group-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(grouped));
    const auto& groupedRecords =
        std::get<std::vector<javelin::jmap::sync::MutationRecord>>(grouped);
    REQUIRE(groupedRecords.size() == 1);
    CHECK(groupedRecords.front().mutationId == "contact-1");

    const auto wrongDomain = repository.listForOperationGroup(
        {.accountId = "account-1", .dataType = "CalendarEvent"}, "group-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(wrongDomain));
    CHECK(std::get<std::vector<javelin::jmap::sync::MutationRecord>>(wrongDomain).empty());
}

TEST_CASE("generic mutation journal preserves ambiguous outcomes across recovery", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::sync::MutationJournalRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository
                      .put({
                          .mutationId = "email-1",
                          .operationGroupId = std::nullopt,
                          .domain = {.accountId = "account-1", .dataType = "Email"},
                          .objectId = "eml-1",
                          .mutationKind = "email_patch",
                          .status = javelin::jmap::sync::MutationStatus::InFlight,
                          .payloadJson = R"({"emailId":"eml-1"})",
                          .baseState = std::nullopt,
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());

    const auto recovered = repository.recoverInFlight();
    REQUIRE(std::holds_alternative<std::size_t>(recovered));
    CHECK(std::get<std::size_t>(recovered) == 1);

    const auto found = repository.find("email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::sync::MutationRecord>>(found));
    const auto& record = std::get<std::optional<javelin::jmap::sync::MutationRecord>>(found);
    REQUIRE(record.has_value());
    CHECK(record->status == javelin::jmap::sync::MutationStatus::Unknown);
    CHECK(javelin::jmap::sync::projectsOptimistically(record->status));

    REQUIRE_FALSE(
        repository.transition("email-1", javelin::jmap::sync::MutationStatus::Accepted, "state-3")
            .has_value());
    const auto accepted = repository.find("email-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::sync::MutationRecord>>(accepted));
    REQUIRE(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(accepted).has_value());
    CHECK(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(accepted)->acceptedState ==
          std::optional<std::string>{"state-3"});
}

TEST_CASE("mutation journal preserves append order independently of mutation ids", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::sync::MutationJournalRepository repository{databaseContext.connection};
    const auto record = [](std::string mutationId, std::string payload)
    {
        return javelin::jmap::sync::MutationRecord{
            .mutationId = std::move(mutationId),
            .operationGroupId = std::nullopt,
            .domain = {.accountId = "account-1", .dataType = "Email"},
            .objectId = "eml-1",
            .mutationKind = "email_patch",
            .status = javelin::jmap::sync::MutationStatus::Pending,
            .payloadJson = std::move(payload),
            .baseState = std::nullopt,
            .acceptedState = std::nullopt,
            .errorJson = std::nullopt,
        };
    };

    REQUIRE_FALSE(repository.put(record("z-first", R"({"order":1})")).has_value());
    REQUIRE_FALSE(repository.put(record("a-second", R"({"order":2})")).has_value());

    const auto result =
        repository.listForObject({.accountId = "account-1", .dataType = "Email"}, "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::MutationRecord>>(result));
    const auto& records = std::get<std::vector<javelin::jmap::sync::MutationRecord>>(result);
    REQUIRE(records.size() == 2);
    CHECK(records[0].mutationId == "z-first");
    CHECK(records[1].mutationId == "a-second");
    CHECK(records[0].sequence < records[1].sequence);
}

TEST_CASE("mutation journal append rejects duplicate ids and missing transitions", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::sync::MutationJournalRepository repository{databaseContext.connection};
    const javelin::jmap::sync::MutationRecord record{
        .mutationId = "immutable-1",
        .operationGroupId = std::nullopt,
        .domain = {.accountId = "account-1", .dataType = "Email"},
        .objectId = "eml-1",
        .mutationKind = "email_patch",
        .status = javelin::jmap::sync::MutationStatus::Pending,
        .payloadJson = R"({"value":1})",
        .baseState = std::nullopt,
        .acceptedState = std::nullopt,
        .errorJson = std::nullopt,
    };
    REQUIRE_FALSE(repository.put(record).has_value());
    auto duplicate = record;
    duplicate.payloadJson = R"({"value":2})";
    CHECK(repository.put(duplicate).has_value());
    CHECK(repository.transition("missing", javelin::jmap::sync::MutationStatus::Accepted)
              .has_value());

    const auto found = repository.find("immutable-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::sync::MutationRecord>>(found));
    REQUIRE(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(found).has_value());
    CHECK(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(found)->payloadJson ==
          R"({"value":1})");
}

TEST_CASE(
    "Email mutation projection rolls back its journal record when cache materialization fails",
    "[jmap][sync][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    QSqlQuery trigger{databaseContext.connection.database()};
    REQUIRE(trigger.exec(
        QStringLiteral("CREATE TRIGGER reject_email_projection BEFORE INSERT ON emails BEGIN "
                       "SELECT RAISE(ABORT,'projection rejected'); END")));

    auto projected = loadEmailFixture();
    projected.id = "eml-atomic";
    projected.mailboxIds = {"mbx-archive"};
    const javelin::jmap::sync::EmailMutationRecord record{
        .mutationId = "atomic-1",
        .operationGroupId = std::nullopt,
        .accountId = "account-1",
        .status = javelin::jmap::sync::MutationStatus::Pending,
        .patch =
            {
                .emailId = "eml-atomic",
                .addMailboxIds = {"mbx-archive"},
                .removeMailboxIds = {"mbx-inbox"},
                .addKeywords = {},
                .removeKeywords = {},
                .destroy = false,
            },
        .baseMailboxIds = std::vector<std::string>{"mbx-inbox"},
        .baseKeywords = std::vector<std::string>{},
        .baseState = std::nullopt,
        .acceptedState = std::nullopt,
        .errorJson = std::nullopt,
    };

    javelin::jmap::sync::EmailMutationJournal emailJournal{databaseContext.connection};
    REQUIRE(emailJournal.queue(record, projected).has_value());

    javelin::jmap::sync::MutationJournalRepository journal{databaseContext.connection};
    const auto found = journal.find("atomic-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::sync::MutationRecord>>(found));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::sync::MutationRecord>>(found).has_value());

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    const auto cached = emails.find("account-1", "eml-atomic");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(cached));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(cached).has_value());
}
