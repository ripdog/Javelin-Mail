#include "jmap/cache/ThreadRepository.h"

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
        return QStringLiteral("javelin-thread-%1").arg(counter);
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

    [[nodiscard]] std::vector<javelin::jmap::domain::Thread> makeThreads()
    {
        return {
            javelin::jmap::domain::Thread{
                .id = "th-1",
                .emailIds = {"em-1", "em-2"},
            },
            javelin::jmap::domain::Thread{
                .id = "th-2",
                .emailIds = {"em-3"},
            },
        };
    }

} // namespace

TEST_CASE("thread repository round-trips stored threads", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::ThreadRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.replaceAll("account-1", makeThreads()).has_value());

    const auto result = repository.find("account-1", "th-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Thread>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Thread>>(result).has_value());
    const auto& thread = *std::get<std::optional<javelin::jmap::domain::Thread>>(result);
    CHECK(thread.id == "th-1");
    REQUIRE(thread.emailIds.size() == 2);
    CHECK(thread.emailIds.front() == "em-1");
    CHECK(thread.emailIds.back() == "em-2");
}

TEST_CASE("thread repository replacement removes stale threads", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::ThreadRepository repository{databaseContext.connection};

    REQUIRE_FALSE(repository.replaceAll("account-1", makeThreads()).has_value());
    REQUIRE_FALSE(repository
                      .replaceAll("account-1", {javelin::jmap::domain::Thread{
                                                   .id = "th-1",
                                                   .emailIds = {"em-1"},
                                               }})
                      .has_value());

    const auto stale = repository.find("account-1", "th-2");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Thread>>(stale));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Thread>>(stale).has_value());
}
