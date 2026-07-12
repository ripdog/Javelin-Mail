#include "jmap/cache/SearchWindowRepository.h"

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

    struct TestDatabaseContext
    {
        QTemporaryDir temporaryDir;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] TestDatabaseContext makeDatabaseContext()
    {
        static int counter = 0;
        TestDatabaseContext context;
        REQUIRE(context.temporaryDir.isValid());
        auto result = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("javelin-search-window-%1").arg(++counter),
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
            "VALUES ('account-1', 'alice@example.com', 'https://mail.example.com/jmap', 1)"));
        REQUIRE(query.exec());
    }
} // namespace

TEST_CASE("search window repository replaces ordered query results", "[jmap][cache][search]")
{
    ApplicationGuard application;
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    javelin::jmap::cache::SearchWindowRepository repository{database.connection};

    REQUIRE_FALSE(repository
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "query-1",
                          .offset = 0,
                          .limit = 100,
                          .total = 3,
                          .emailIds = {"email-2", "email-1", "email-3"},
                      })
                      .has_value());

    const auto firstResult = repository.find("account-1", "query-1", 0, 100);
    const auto* first =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&firstResult);
    REQUIRE(first != nullptr);
    REQUIRE(first->has_value());
    CHECK((*first)->total == std::optional<std::size_t>{3});
    CHECK((*first)->emailIds == std::vector<std::string>{"email-2", "email-1", "email-3"});

    REQUIRE_FALSE(repository
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "query-1",
                          .offset = 0,
                          .limit = 100,
                          .total = 1,
                          .emailIds = {"email-4"},
                      })
                      .has_value());

    const auto replacedResult = repository.find("account-1", "query-1", 0, 100);
    const auto* replaced =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&replacedResult);
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->has_value());
    CHECK((*replaced)->total == std::optional<std::size_t>{1});
    CHECK((*replaced)->emailIds == std::vector<std::string>{"email-4"});
}

TEST_CASE("search window repository distinguishes pages and missing windows",
          "[jmap][cache][search]")
{
    ApplicationGuard application;
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    javelin::jmap::cache::SearchWindowRepository repository{database.connection};

    REQUIRE_FALSE(repository
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "query-1",
                          .offset = 100,
                          .limit = 100,
                          .total = std::nullopt,
                          .emailIds = {"email-101"},
                      })
                      .has_value());

    const auto foundResult = repository.find("account-1", "query-1", 100, 100);
    const auto* found =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&foundResult);
    REQUIRE(found != nullptr);
    REQUIRE(found->has_value());
    CHECK_FALSE((*found)->total.has_value());

    const auto missingResult = repository.find("account-1", "query-1", 0, 100);
    const auto* missing =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&missingResult);
    REQUIRE(missing != nullptr);
    CHECK_FALSE(missing->has_value());
}
