#include "jmap/cache/EmailRepository.h"
#include "FixtureReader.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/domain/MailEntityParsers.h"

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
        return QStringLiteral("javelin-email-%1").arg(counter);
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

    void seedMessageContent(javelin::jmap::cache::DatabaseConnection& connection,
                            const QString& emailId)
    {
        QSqlQuery sourceQuery{connection.database()};
        sourceQuery.prepare(QStringLiteral("INSERT INTO raw_message_sources ("
                                           "account_id, email_id, blob_id, payload"
                                           ") VALUES ("
                                           ":account_id, :email_id, :blob_id, :payload)"));
        sourceQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        sourceQuery.bindValue(QStringLiteral(":email_id"), emailId);
        sourceQuery.bindValue(QStringLiteral(":blob_id"), QStringLiteral("blob-root"));
        sourceQuery.bindValue(QStringLiteral(":payload"),
                              QByteArrayLiteral("Subject: Cached\r\n\r\nBody"));
        REQUIRE(sourceQuery.exec());
    }

    [[nodiscard]] int rawSourceCount(javelin::jmap::cache::DatabaseConnection& connection,
                                     const QString& emailId)
    {
        QSqlQuery sourceQuery{connection.database()};
        sourceQuery.prepare(QStringLiteral(
            "SELECT COUNT(*) FROM raw_message_sources WHERE account_id = :account_id AND "
            "email_id = :email_id"));
        sourceQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        sourceQuery.bindValue(QStringLiteral(":email_id"), emailId);
        REQUIRE(sourceQuery.exec());
        REQUIRE(sourceQuery.next());
        return sourceQuery.value(0).toInt();
    }

} // namespace

TEST_CASE("email repository round-trips cached email summaries", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};
    const auto email = loadEmailFixture();

    REQUIRE_FALSE(repository.replaceAll("account-1", {email}).has_value());

    const auto result = repository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(result).has_value());
    const auto& loaded = *std::get<std::optional<javelin::jmap::domain::Email>>(result);
    CHECK(loaded.threadId == "thr-123");
    CHECK(loaded.blobId == "blob-1");
    CHECK(loaded.hasAttachment);
    CHECK(loaded.subject == email.subject);
    CHECK(loaded.preview == email.preview);
    CHECK(loaded.mailboxIds == std::vector<std::string>{"mbx-inbox"});
    CHECK(loaded.keywords == std::vector<std::string>{"$flagged", "$seen"});
    REQUIRE(loaded.from.size() == 1);
    CHECK(loaded.from.front().email == "alice@example.com");
    REQUIRE(loaded.replyTo.size() == 1);
    CHECK(loaded.replyTo.front().email == "support@example.com");
}

TEST_CASE("email repository reports whether an account has cached Email rows",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    const auto empty = repository.hasAny("account-1");
    REQUIRE(std::holds_alternative<bool>(empty));
    CHECK_FALSE(std::get<bool>(empty));

    REQUIRE_FALSE(repository.replaceAll("account-1", {loadEmailFixture()}).has_value());
    const auto populated = repository.hasAny("account-1");
    REQUIRE(std::holds_alternative<bool>(populated));
    CHECK(std::get<bool>(populated));

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Check Email existence"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    const auto inTransaction = repository.hasAny(transaction, "account-1");
    REQUIRE(std::holds_alternative<bool>(inTransaction));
    CHECK(std::get<bool>(inTransaction));
    REQUIRE_FALSE(transaction.commit().has_value());
}

TEST_CASE("email repository normalizes missing subject and preview for cache writes",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto email = loadEmailFixture();
    email.subject = std::nullopt;
    email.preview = std::nullopt;

    REQUIRE_FALSE(repository.replaceAll("account-1", {email}).has_value());
    REQUIRE_FALSE(repository.upsertMany("account-1", {email}).has_value());

    const auto result = repository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(result).has_value());
    const auto& loaded = *std::get<std::optional<javelin::jmap::domain::Email>>(result);
    CHECK(loaded.subject == std::optional<std::string>{std::string{}});
    CHECK(loaded.preview == std::optional<std::string>{std::string{}});
}

TEST_CASE("email upserts stale incomplete current Thread membership",
          "[jmap][cache][repository][thread-materialization]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    javelin::jmap::cache::ThreadRepository threads{databaseContext.connection};

    auto first = loadEmailFixture();
    REQUIRE_FALSE(emails.upsertMany("account-1", {first}).has_value());
    REQUIRE_FALSE(threads.upsertMany("account-1", {{.id = first.threadId, .emailIds = {first.id}}})
                      .has_value());

    REQUIRE_FALSE(emails.upsertMany("account-1", {first}).has_value());
    auto membership = threads.findMembership("account-1", first.threadId);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membership));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
              ->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Current);

    auto reply = first;
    reply.id = "eml-2";
    reply.subject = "Reply";
    REQUIRE_FALSE(emails.upsertMany("account-1", {reply}).has_value());

    membership = threads.findMembership("account-1", first.threadId);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membership));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
              ->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Stale);
}

TEST_CASE("email repository records bodyless messages as indexed without a null preview",
          "[jmap][cache][repository][search]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};
    auto email = loadEmailFixture();
    email.preview = std::nullopt;
    REQUIRE_FALSE(repository.replaceAll("account-1", {email}).has_value());

    QSqlQuery seed{databaseContext.connection.database()};
    REQUIRE(
        seed.exec(QStringLiteral("INSERT INTO mail_vault_objects(content_hash,relative_path,size) "
                                 "VALUES('bodyless-hash','objects/bodyless',0)")));
    REQUIRE(seed.exec(QStringLiteral(
        "INSERT INTO mail_vault_email_refs(account_id,email_id,blob_id,content_hash,retention) "
        "VALUES('account-1','eml-1','blob-1','bodyless-hash','evictable')")));

    REQUIRE_FALSE(
        repository.markSearchIndexed("account-1", "eml-1", "bodyless-hash", {}).has_value());

    QSqlQuery result{databaseContext.connection.database()};
    REQUIRE(result.exec(QStringLiteral(
        "SELECT e.preview,e.preview IS NULL,r.indexed_hash,r.body_preview FROM emails e JOIN "
        "mail_vault_email_refs r ON r.account_id=e.account_id AND r.email_id=e.email_id "
        "WHERE e.account_id='account-1' AND e.email_id='eml-1'")));
    REQUIRE(result.next());
    CHECK(result.value(0).toString() == QStringLiteral(""));
    CHECK_FALSE(result.value(1).toBool());
    CHECK(result.value(2).toString() == QStringLiteral("bodyless-hash"));
    CHECK(result.value(3).toString().isEmpty());
}

TEST_CASE("email repository replacement removes stale email rows", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto first = loadEmailFixture();
    auto second = first;
    second.id = "eml-2";
    second.subject = "Second";

    REQUIRE_FALSE(repository.replaceAll("account-1", {first, second}).has_value());
    REQUIRE_FALSE(repository.replaceAll("account-1", {first}).has_value());

    const auto stale = repository.find("account-1", "eml-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(stale));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(stale).has_value());
}

TEST_CASE(
    "email repository replacement preserves cached message content independently of summaries",
    "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto first = loadEmailFixture();
    auto second = first;
    second.id = "eml-2";

    REQUIRE_FALSE(repository.replaceAll("account-1", {first, second}).has_value());
    seedMessageContent(databaseContext.connection, QStringLiteral("eml-1"));
    seedMessageContent(databaseContext.connection, QStringLiteral("eml-2"));

    REQUIRE_FALSE(repository.replaceAll("account-1", {first}).has_value());

    CHECK(rawSourceCount(databaseContext.connection, QStringLiteral("eml-1")) == 1);
    CHECK(rawSourceCount(databaseContext.connection, QStringLiteral("eml-2")) == 1);
}

TEST_CASE("email repository upsert preserves cached message content for existing emails",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto email = loadEmailFixture();
    REQUIRE_FALSE(repository.replaceAll("account-1", {email}).has_value());
    seedMessageContent(databaseContext.connection, QStringLiteral("eml-1"));

    email.subject = "Updated subject";
    REQUIRE_FALSE(repository.upsertMany("account-1", {email}).has_value());

    CHECK(rawSourceCount(databaseContext.connection, QStringLiteral("eml-1")) == 1);
}

TEST_CASE("email repository removal clears cached raw message sources", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto email = loadEmailFixture();
    REQUIRE_FALSE(repository.replaceAll("account-1", {email}).has_value());
    seedMessageContent(databaseContext.connection, QStringLiteral("eml-1"));

    const std::vector<std::string> emailIds{"eml-1"};
    REQUIRE_FALSE(repository.removeMany("account-1", emailIds).has_value());

    CHECK(rawSourceCount(databaseContext.connection, QStringLiteral("eml-1")) == 0);
}

TEST_CASE("email repository reports which email ids already exist", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto first = loadEmailFixture();
    auto second = first;
    second.id = "eml-2";

    REQUIRE_FALSE(repository.replaceAll("account-1", {first, second}).has_value());

    const auto result = repository.existingIds(
        "account-1", std::vector<std::string>{"eml-2", "eml-missing", "eml-1"});
    REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
    CHECK(std::get<std::vector<std::string>>(result) == std::vector<std::string>{"eml-2", "eml-1"});
}

TEST_CASE("email repository lists mailbox email ids", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto inboxEmail = loadEmailFixture();
    inboxEmail.id = "eml-inbox";
    inboxEmail.mailboxIds = {"mbx-inbox"};

    auto archivedEmail = inboxEmail;
    archivedEmail.id = "eml-archive";
    archivedEmail.mailboxIds = {"mbx-archive"};

    auto bothMailboxes = inboxEmail;
    bothMailboxes.id = "eml-both";
    bothMailboxes.mailboxIds = {"mbx-inbox", "mbx-archive"};

    REQUIRE_FALSE(
        repository.replaceAll("account-1", {inboxEmail, archivedEmail, bothMailboxes}).has_value());

    const auto result = repository.listMailboxEmailIds("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(result));
    CHECK(std::get<std::vector<std::string>>(result) ==
          std::vector<std::string>{"eml-both", "eml-inbox"});
}

TEST_CASE("email repository removes mailbox membership without deleting email",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::EmailRepository repository{databaseContext.connection};

    auto email = loadEmailFixture();
    email.id = "eml-both";
    email.mailboxIds = {"mbx-inbox", "mbx-archive"};

    REQUIRE_FALSE(repository.replaceAll("account-1", {email}).has_value());

    const std::vector<std::string> emailIds{"eml-both"};
    REQUIRE_FALSE(repository.removeFromMailbox("account-1", "mbx-inbox", emailIds).has_value());

    const auto inboxResult = repository.listMailboxEmailIds("account-1", "mbx-inbox");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(inboxResult));
    CHECK(std::get<std::vector<std::string>>(inboxResult).empty());

    const auto loadedResult = repository.find("account-1", "eml-both");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(loadedResult));
    const auto& loaded = std::get<std::optional<javelin::jmap::domain::Email>>(loadedResult);
    REQUIRE(loaded.has_value());
    CHECK(loaded->mailboxIds == std::vector<std::string>{"mbx-archive"});
}
