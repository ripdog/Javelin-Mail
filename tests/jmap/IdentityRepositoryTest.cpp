#include "jmap/cache/IdentityRepository.h"
#include "jmap/sync/MutationJournal.h"

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
                return;
            static int argc = 1;
            static char appName[] = "javelin-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] QString connectionName()
    {
        static int counter = 0;
        return QStringLiteral("javelin-identity-repository-%1").arg(++counter);
    }

    struct DatabaseContext
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] DatabaseContext makeDatabase()
    {
        DatabaseContext context;
        REQUIRE(context.directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = connectionName(),
            .databasePath = context.directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        context.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        return context;
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.com','https://mail.example.test/jmap',1)"));
        REQUIRE(query.exec());
    }

    [[nodiscard]] javelin::jmap::domain::Identity identity(std::string id, std::string name,
                                                           std::string signature)
    {
        return {
            .id = std::move(id),
            .name = std::move(name),
            .email = "alice@example.com",
            .replyTo = {},
            .bcc = {},
            .textSignature = signature,
            .htmlSignature = "<p>" + signature + "</p>",
            .mayDelete = true,
        };
    }
} // namespace

TEST_CASE("identity repository stores duplicate-address identities with their state token",
          "[jmap][identity][repository]")
{
    ApplicationGuard guard;
    Q_UNUSED(guard);
    auto database = makeDatabase();
    seedAccount(database.connection);
    javelin::jmap::cache::IdentityRepository repository{database.connection};

    REQUIRE_FALSE(repository
                      .replaceAll("account-1",
                                  {identity("identity-personal", "Alice", "Personal"),
                                   identity("identity-work", "Alice", "Work")},
                                  "identity-state-1")
                      .has_value());

    const auto listed = repository.listByAccount("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(listed));
    const auto& identities = std::get<std::vector<javelin::jmap::domain::Identity>>(listed);
    REQUIRE(identities.size() == 2);
    CHECK(identities.at(0).email == "alice@example.com");
    CHECK(identities.at(1).email == "alice@example.com");
    CHECK(identities.at(0).id != identities.at(1).id);

    const auto state = repository.state("account-1");
    REQUIRE(std::holds_alternative<std::optional<std::string>>(state));
    CHECK(std::get<std::optional<std::string>>(state) ==
          std::optional<std::string>{"identity-state-1"});
}

TEST_CASE("identity repository projects updates and destroys inside caller transactions",
          "[jmap][identity][repository]")
{
    ApplicationGuard guard;
    Q_UNUSED(guard);
    auto database = makeDatabase();
    seedAccount(database.connection);
    javelin::jmap::cache::IdentityRepository repository{database.connection};
    REQUIRE_FALSE(repository
                      .replaceAll("account-1",
                                  {identity("identity-personal", "Alice", "Personal"),
                                   identity("identity-work", "Alice", "Work")},
                                  "identity-state-1")
                      .has_value());

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        database.connection, QStringLiteral("Project identities"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    auto updated = identity("identity-work", "Alice Example Ltd", "Updated work");
    REQUIRE_FALSE(repository.projectUpsert(transaction, "account-1", updated).has_value());
    REQUIRE_FALSE(
        repository.projectDestroy(transaction, "account-1", "identity-personal").has_value());
    REQUIRE_FALSE(transaction.commit().has_value());

    const auto listed = repository.listByAccount("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(listed));
    const auto& identities = std::get<std::vector<javelin::jmap::domain::Identity>>(listed);
    REQUIRE(identities.size() == 1);
    CHECK(identities.front().id == "identity-work");
    CHECK(identities.front().name == "Alice Example Ltd");
    CHECK(identities.front().textSignature == std::optional<std::string>{"Updated work"});
}

TEST_CASE("pending identity creates remain separate from confirmed server identities",
          "[jmap][identity][repository]")
{
    ApplicationGuard guard;
    Q_UNUSED(guard);
    auto database = makeDatabase();
    seedAccount(database.connection);
    javelin::jmap::cache::IdentityRepository repository{database.connection};
    REQUIRE_FALSE(repository
                      .replaceAll("account-1", {identity("identity-1", "Alice", "Existing")},
                                  "identity-state-1")
                      .has_value());

    auto transactionResult = javelin::jmap::sync::MutationProjectionTransaction::begin(
        database.connection, QStringLiteral("Project Identity create"));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::MutationProjectionTransaction>(
        transactionResult));
    auto transaction =
        std::get<javelin::jmap::sync::MutationProjectionTransaction>(std::move(transactionResult));
    REQUIRE_FALSE(transaction
                      .append({
                          .mutationId = "mutation-1",
                          .operationGroupId = std::nullopt,
                          .domain = {.accountId = "account-1", .dataType = "Identity"},
                          .objectId = "creation-1",
                          .mutationKind = "identity_create",
                          .status = javelin::jmap::sync::MutationStatus::Pending,
                          .payloadJson = "{}",
                          .baseState = "identity-state-1",
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());
    REQUIRE_FALSE(repository
                      .projectPendingCreate(transaction.cacheTransaction(), "account-1",
                                            "creation-1", "mutation-1",
                                            identity({}, "Alice Work", "Work signature"))
                      .has_value());
    REQUIRE_FALSE(transaction.commit().has_value());

    const auto confirmed = repository.listByAccount("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::domain::Identity>>(confirmed));
    REQUIRE(std::get<std::vector<javelin::jmap::domain::Identity>>(confirmed).size() == 1);

    const auto pending = repository.listPendingCreates("account-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(pending));
    const auto& creates =
        std::get<std::vector<javelin::jmap::cache::PendingIdentityCreate>>(pending);
    REQUIRE(creates.size() == 1);
    CHECK(creates.front().creationId == "creation-1");
    CHECK(creates.front().identity.id.empty());
    CHECK(creates.front().identity.textSignature == std::optional<std::string>{"Work signature"});
    CHECK(creates.front().status == "pending");
}
