#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/InlinePartPayloadRepository.h"
#include "jmap/cache/MessageContentRepository.h"
#include "FixtureReader.h"
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
        QSqlQuery partQuery{connection.database()};
        partQuery.prepare(QStringLiteral(
            "INSERT INTO email_parts ("
            "account_id, email_id, part_id, parent_part_id, blob_id, kind, media_type, name, "
            "charset, disposition, cid, size, is_inline_renderable, is_body_section"
            ") VALUES ("
            ":account_id, :email_id, :part_id, :parent_part_id, :blob_id, :kind, :media_type, "
            ":name, :charset, :disposition, :cid, :size, :is_inline_renderable, "
            ":is_body_section)"));
        partQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        partQuery.bindValue(QStringLiteral(":email_id"), emailId);
        partQuery.bindValue(QStringLiteral(":part_id"), QStringLiteral("1"));
        partQuery.bindValue(QStringLiteral(":parent_part_id"), QVariant{});
        partQuery.bindValue(QStringLiteral(":blob_id"), QStringLiteral("blob-html"));
        partQuery.bindValue(QStringLiteral(":kind"), QStringLiteral("body"));
        partQuery.bindValue(QStringLiteral(":media_type"), QStringLiteral("text/html"));
        partQuery.bindValue(QStringLiteral(":name"), QVariant{});
        partQuery.bindValue(QStringLiteral(":charset"), QStringLiteral("utf-8"));
        partQuery.bindValue(QStringLiteral(":disposition"), QVariant{});
        partQuery.bindValue(QStringLiteral(":cid"), QVariant{});
        partQuery.bindValue(QStringLiteral(":size"), 128);
        partQuery.bindValue(QStringLiteral(":is_inline_renderable"), 0);
        partQuery.bindValue(QStringLiteral(":is_body_section"), 1);
        REQUIRE(partQuery.exec());

        QSqlQuery bodyQuery{connection.database()};
        bodyQuery.prepare(
            QStringLiteral("INSERT INTO email_body_values ("
                           "account_id, email_id, part_id, blob_id, is_truncated, value"
                           ") VALUES ("
                           ":account_id, :email_id, :part_id, :blob_id, :is_truncated, :value)"));
        bodyQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        bodyQuery.bindValue(QStringLiteral(":email_id"), emailId);
        bodyQuery.bindValue(QStringLiteral(":part_id"), QStringLiteral("1"));
        bodyQuery.bindValue(QStringLiteral(":blob_id"), QStringLiteral("blob-html"));
        bodyQuery.bindValue(QStringLiteral(":is_truncated"), 0);
        bodyQuery.bindValue(QStringLiteral(":value"), QStringLiteral("<p>Cached body</p>"));
        REQUIRE(bodyQuery.exec());
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

TEST_CASE("email repository replacement preserves cached message content for retained emails",
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

    javelin::jmap::cache::MessageContentRepository contentRepository{databaseContext.connection};
    const auto retainedBodies = contentRepository.loadBodyValues("account-1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::EmailBodyValue>>(retainedBodies));
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(retainedBodies).size() == 1);

    const auto removedBodies = contentRepository.loadBodyValues("account-1", "eml-2");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::EmailBodyValue>>(removedBodies));
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(removedBodies).empty());
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

    javelin::jmap::cache::MessageContentRepository contentRepository{databaseContext.connection};
    const auto bodies = contentRepository.loadBodyValues("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::EmailBodyValue>>(bodies));
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(bodies).size() == 1);
}

TEST_CASE("email repository removal clears cached message content and inline payloads",
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

    javelin::jmap::cache::InlinePartPayloadRepository payloadRepository{databaseContext.connection};
    REQUIRE_FALSE(payloadRepository
                      .upsert("account-1",
                              {
                                  .emailId = "eml-1",
                                  .partId = "2",
                                  .blobId = "blob-inline",
                                  .mediaType = "image/png",
                                  .payload = QByteArrayLiteral("PNGDATA"),
                              })
                      .has_value());

    const std::vector<std::string> emailIds{"eml-1"};
    REQUIRE_FALSE(repository.removeMany("account-1", emailIds).has_value());

    javelin::jmap::cache::MessageContentRepository contentRepository{databaseContext.connection};
    const auto bodies = contentRepository.loadBodyValues("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::EmailBodyValue>>(bodies));
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(bodies).empty());

    const auto payload = payloadRepository.find("account-1", "eml-1", "2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::InlinePartPayload>>(payload));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::InlinePartPayload>>(payload).has_value());
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
    CHECK(std::get<std::vector<std::string>>(result) ==
          std::vector<std::string>{"eml-2", "eml-1"});
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
