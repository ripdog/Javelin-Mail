#include "jmap/cache/SyncStateRepository.h"

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
        return QStringLiteral("javelin-sync-state-%1").arg(counter);
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

    [[nodiscard]] javelin::jmap::cache::SyncStateKey makeKey()
    {
        return javelin::jmap::cache::SyncStateKey{
            .accountId = "account-1",
            .objectType = "Email",
            .queryKey = "mailbox:inbox",
        };
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

} // namespace

TEST_CASE("sync state repository round-trips stored state tokens", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::SyncStateRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.upsert(makeKey(), "state-1").has_value());

    const auto result = repository.find(makeKey());
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result).has_value());
    const auto& record = *std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result);
    CHECK(record.key.accountId == "account-1");
    CHECK(record.key.objectType == "Email");
    CHECK(record.key.queryKey == "mailbox:inbox");
    CHECK(record.stateToken == "state-1");
    CHECK_FALSE(record.updatedAt.empty());
}

TEST_CASE("sync state repository updates existing state tokens in place",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::SyncStateRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.upsert(makeKey(), "state-1").has_value());
    REQUIRE_FALSE(repository.upsert(makeKey(), "state-2").has_value());

    const auto result = repository.find(makeKey());
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result)->stateToken ==
          "state-2");

    QSqlQuery countQuery{databaseContext.connection.database()};
    REQUIRE(countQuery.exec(QStringLiteral(
        "SELECT COUNT(*) FROM sync_state WHERE account_id = 'account-1' AND object_type = "
        "'Email' AND query_key = 'mailbox:inbox'")));
    REQUIRE(countQuery.next());
    CHECK(countQuery.value(0).toInt() == 1);
}

TEST_CASE("sync state repository removes stored state tokens", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::SyncStateRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.upsert(makeKey(), "state-1").has_value());
    REQUIRE_FALSE(repository.remove(makeKey()).has_value());

    const auto result = repository.find(makeKey());
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(result));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(result).has_value());
}
