#include "jmap/sync/SyncReconciler.h"

#include "FixtureReader.h"
#include "jmap/cache/MessageContentRepository.h"
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
        return QStringLiteral("javelin-sync-reconcile-%1").arg(counter);
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

} // namespace

TEST_CASE("sync reconciler applies mailbox upserts deletions and state updates", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MailboxRepository mailboxRepository{databaseContext.connection};
    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    const javelin::jmap::sync::SyncReconciler reconciler{mailboxRepository, emailRepository,
                                                         syncStateRepository};

    auto staleMailbox = loadMailboxFixture();
    staleMailbox.id = "mbx-stale";
    staleMailbox.name = "Stale";
    REQUIRE_FALSE(mailboxRepository.replaceAll("account-1", {staleMailbox}).has_value());

    auto updatedMailbox = loadMailboxFixture();
    updatedMailbox.name = "Inbox Renamed";
    const javelin::jmap::api::ChangesResponse changes{
        .accountId = "account-1",
        .oldState = "state-1",
        .newState = "state-2",
        .hasMoreChanges = false,
        .created = {},
        .updated = {updatedMailbox.id},
        .destroyed = {"mbx-stale"},
    };
    const javelin::jmap::api::MailboxGetResponse fetched{
        .accountId = "account-1",
        .state = "state-2",
        .list = {updatedMailbox},
        .notFound = {},
    };
    const javelin::jmap::cache::SyncStateKey key{
        .accountId = "account-1",
        .objectType = "Mailbox",
        .queryKey = "",
    };

    REQUIRE_FALSE(reconciler.applyMailboxChanges(key, changes, fetched).has_value());

    const auto roots = mailboxRepository.listByParent("account-1", std::nullopt);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Mailbox>>(roots));
    const auto& items = std::get<std::vector<javelin::jmap::domain::Mailbox>>(roots);
    REQUIRE(items.size() == 1);
    CHECK(items.front().id == "mbx-inbox");
    CHECK(items.front().name == "Inbox Renamed");

    const auto state = syncStateRepository.find(key);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(state));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state)->stateToken ==
          "state-2");
}

TEST_CASE("sync reconciler deletes stale email rows and their cached MIME content", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MailboxRepository mailboxRepository{databaseContext.connection};
    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    javelin::jmap::cache::MessageContentRepository contentRepository{databaseContext.connection};
    const javelin::jmap::sync::SyncReconciler reconciler{mailboxRepository, emailRepository,
                                                         syncStateRepository};

    auto staleEmail = loadEmailFixture();
    staleEmail.id = "eml-stale";
    staleEmail.subject = "Old subject";
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {staleEmail}).has_value());
    REQUIRE_FALSE(contentRepository
                      .replaceForEmail("account-1", "eml-stale",
                                       {javelin::jmap::cache::EmailPart{
                                           .emailId = "eml-stale",
                                           .partId = "1",
                                           .parentPartId = std::nullopt,
                                           .blobId = "blob-stale",
                                           .kind = "attachment",
                                           .mediaType = "image/png",
                                           .name = "stale.png",
                                           .charset = std::nullopt,
                                           .disposition = std::optional<std::string>{"attachment"},
                                           .cid = std::nullopt,
                                           .size = 123,
                                           .isInlineRenderable = false,
                                           .isBodySection = false,
                                       }},
                                       {javelin::jmap::cache::EmailBodyValue{
                                           .emailId = "eml-stale",
                                           .partId = "1.1",
                                           .blobId = std::nullopt,
                                           .isTruncated = false,
                                           .value = "stale body",
                                       }})
                      .has_value());

    auto updatedEmail = loadEmailFixture();
    updatedEmail.subject = "Updated subject";
    const javelin::jmap::api::ChangesResponse changes{
        .accountId = "account-1",
        .oldState = "state-1",
        .newState = "state-2",
        .hasMoreChanges = false,
        .created = {},
        .updated = {updatedEmail.id},
        .destroyed = {"eml-stale"},
    };
    const javelin::jmap::api::EmailGetResponse fetched{
        .accountId = "account-1",
        .state = "state-2",
        .list = {updatedEmail},
        .notFound = {},
    };
    const javelin::jmap::cache::SyncStateKey key{
        .accountId = "account-1",
        .objectType = "Email",
        .queryKey = "mailbox:mbx-inbox",
    };

    REQUIRE_FALSE(reconciler.applyEmailChanges(key, changes, fetched).has_value());

    const auto updated = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(updated));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(updated).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(updated)->subject ==
          std::optional<std::string>{"Updated subject"});

    const auto stale = emailRepository.find("account-1", "eml-stale");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(stale));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(stale).has_value());
    const auto parts = contentRepository.loadParts("account-1", "eml-stale");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::EmailPart>>(parts));
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailPart>>(parts).empty());

    const auto bodyValues = contentRepository.loadBodyValues("account-1", "eml-stale");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::EmailBodyValue>>(bodyValues));
    CHECK(std::get<std::vector<javelin::jmap::cache::EmailBodyValue>>(bodyValues).empty());

    const auto state = syncStateRepository.find(key);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(state));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(state)->stateToken ==
          "state-2");
}
