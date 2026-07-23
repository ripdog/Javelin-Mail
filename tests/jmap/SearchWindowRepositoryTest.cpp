#include "jmap/cache/SearchWindowRepository.h"
#include "jmap/cache/MailboxWindowRepository.h"

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
                          .position = 0,
                          .returnedLimit = 100,
                          .total = 3,
                          .queryState = "state-1",
                          .emailIds = {"email-2", "email-1", "email-3"},
                      })
                      .has_value());

    const auto firstResult = repository.find("account-1", "query-1", 0, 100);
    const auto* first =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&firstResult);
    REQUIRE(first != nullptr);
    REQUIRE(first->has_value());
    CHECK((*first)->position == 0);
    CHECK((*first)->returnedLimit == 100);
    CHECK((*first)->total == std::optional<std::size_t>{3});
    CHECK((*first)->queryState == "state-1");
    CHECK((*first)->isAuthoritative);
    CHECK((*first)->emailIds == std::vector<std::string>{"email-2", "email-1", "email-3"});

    REQUIRE_FALSE(repository
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "query-1",
                          .offset = 100,
                          .limit = 100,
                          .position = 100,
                          .returnedLimit = 100,
                          .total = 3,
                          .queryState = "state-1",
                          .emailIds = {"email-101"},
                      })
                      .has_value());

    REQUIRE_FALSE(repository
                      .replace({
                          .accountId = "account-1",
                          .queryKey = "query-1",
                          .offset = 0,
                          .limit = 100,
                          .position = 0,
                          .returnedLimit = 50,
                          .total = 1,
                          .queryState = "state-2",
                          .emailIds = {"email-4"},
                      })
                      .has_value());

    const auto replacedResult = repository.find("account-1", "query-1", 0, 100);
    const auto* replaced =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&replacedResult);
    REQUIRE(replaced != nullptr);
    REQUIRE(replaced->has_value());
    CHECK((*replaced)->returnedLimit == 50);
    CHECK((*replaced)->total == std::optional<std::size_t>{1});
    CHECK((*replaced)->queryState == "state-2");
    CHECK((*replaced)->emailIds == std::vector<std::string>{"email-4"});
    const auto staleSiblingResult = repository.find("account-1", "query-1", 100, 100);
    const auto* staleSibling =
        std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&staleSiblingResult);
    REQUIRE(staleSibling != nullptr);
    CHECK_FALSE(staleSibling->has_value());
}

TEST_CASE("search window invalidation retains every page as a stale identity window",
          "[jmap][cache][search]")
{
    ApplicationGuard application;
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    javelin::jmap::cache::SearchWindowRepository repository{database.connection};

    for (const auto offset : {std::size_t{0}, std::size_t{100}})
    {
        REQUIRE_FALSE(repository
                          .replace({
                              .accountId = "account-1",
                              .queryKey = "query-1",
                              .offset = offset,
                              .limit = 100,
                              .position = offset,
                              .returnedLimit = 100,
                              .total = 200,
                              .queryState = "state-1",
                              .emailIds = {"email-1"},
                          })
                          .has_value());
    }

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        database.connection, QStringLiteral("Invalidate search test"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    REQUIRE_FALSE(repository.invalidateAccount(transaction, "account-1").has_value());
    REQUIRE_FALSE(transaction.commit().has_value());

    for (const auto offset : {std::size_t{0}, std::size_t{100}})
    {
        const auto result = repository.find("account-1", "query-1", offset, 100);
        const auto* window =
            std::get_if<std::optional<javelin::jmap::cache::SearchWindowRecord>>(&result);
        REQUIRE(window != nullptr);
        REQUIRE(window->has_value());
        CHECK_FALSE((*window)->isAuthoritative);
        CHECK((*window)->offset == offset);
        CHECK((*window)->emailIds == std::vector<std::string>{"email-1"});
    }
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
                          .position = 100,
                          .returnedLimit = 100,
                          .total = std::nullopt,
                          .queryState = "state-1",
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

TEST_CASE("search window repository removes one promoted search session", "[jmap][cache][search]")
{
    ApplicationGuard application;
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    javelin::jmap::cache::SearchWindowRepository repository{database.connection};

    for (const auto key : {"query|session:one", "query|session:two"})
    {
        REQUIRE_FALSE(repository
                          .replace({
                              .accountId = "account-1",
                              .queryKey = key,
                              .offset = 0,
                              .limit = 100,
                              .position = 0,
                              .returnedLimit = 100,
                              .total = 1,
                              .queryState = "state-1",
                              .emailIds = {"email-1"},
                          })
                          .has_value());
    }

    REQUIRE_FALSE(repository.eraseQuery("account-1", "query|session:one").has_value());
    const auto erased = repository.find("account-1", "query|session:one", 0, 100);
    const auto retained = repository.find("account-1", "query|session:two", 0, 100);
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SearchWindowRecord>>(erased) ==
            std::nullopt);
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::SearchWindowRecord>>(retained).has_value());
}

TEST_CASE("mailbox windows preserve exact sparse server positions and invalidate by mailbox",
          "[jmap][cache][mailbox-window]")
{
    ApplicationGuard application;
    auto database = makeDatabaseContext();
    seedAccount(database.connection);
    javelin::jmap::cache::MailboxWindowRepository repository{database.connection};

    REQUIRE_FALSE(repository
                      .replace({
                          .accountId = "account-1",
                          .mailboxId = "mbx-inbox",
                          .queryKey = "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true",
                          .requestedOffset = 200,
                          .requestedLimit = 100,
                          .position = 200,
                          .returnedLimit = 50,
                          .total = 1200,
                          .queryState = "query-state-7",
                          .emailIds = {"email-201", "email-202"},
                      })
                      .has_value());

    const auto result = repository.find(
        "account-1", "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true", 200, 100);
    const auto* found =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(&result);
    REQUIRE(found != nullptr);
    REQUIRE(found->has_value());
    CHECK((*found)->position == 200);
    CHECK((*found)->returnedLimit == 50);
    CHECK((*found)->total == std::optional<std::size_t>{1200});
    CHECK((*found)->queryState == "query-state-7");
    CHECK((*found)->emailIds == std::vector<std::string>{"email-201", "email-202"});

    const auto adjacent = repository.find(
        "account-1", "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true", 100, 100);
    const auto* missing =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(&adjacent);
    REQUIRE(missing != nullptr);
    CHECK_FALSE(missing->has_value());

    REQUIRE_FALSE(repository.invalidateMailbox("account-1", "mbx-inbox").has_value());
    const auto invalidated = repository.find(
        "account-1", "mailbox:mbx-inbox|sort:receivedAt:desc|collapseThreads:true", 200, 100);
    const auto* stale =
        std::get_if<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(&invalidated);
    REQUIRE(stale != nullptr);
    REQUIRE(stale->has_value());
    CHECK_FALSE((*stale)->isAuthoritative);
    CHECK((*stale)->emailIds == std::vector<std::string>{"email-201", "email-202"});
}
