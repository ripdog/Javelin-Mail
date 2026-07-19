#include "app/MailboxSyncCoverage.h"

#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoreApplication>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    class ApplicationGuard
    {
      public:
        ApplicationGuard()
        {
            if (QCoreApplication::instance() == nullptr)
            {
                static int argc = 1;
                static char name[] = "mailbox-sync-coverage-test";
                static char* argv[] = {name, nullptr};
                m_application = std::make_unique<QCoreApplication>(argc, argv);
            }
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    struct DatabaseContext
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    DatabaseContext makeDatabaseContext()
    {
        DatabaseContext context;
        REQUIRE(context.directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QUuid::createUuid().toString(QUuid::WithoutBraces),
            .databasePath = context.directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
        context.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        QSqlQuery account{context.connection.database()};
        REQUIRE(account.exec(QStringLiteral(
            "INSERT INTO accounts (account_id,email_address,session_url,is_primary) "
            "VALUES ('account-1','alice@example.com','https://mail.example.com/jmap',1)")));
        return context;
    }

    void storeWindow(javelin::jmap::cache::DatabaseConnection& connection,
                     const std::string& mailboxId)
    {
        javelin::jmap::cache::MailboxWindowRepository windows{connection};
        REQUIRE_FALSE(windows
                          .replace({
                              .accountId = "account-1",
                              .mailboxId = mailboxId,
                              .queryKey = javelin::jmap::sync::mailboxQueryKey({
                                  .mailboxId = mailboxId,
                                  .sortProperty = "receivedAt",
                                  .isAscending = false,
                                  .collapseThreads = true,
                              }),
                              .requestedOffset = 0,
                              .requestedLimit = 100,
                              .position = 0,
                              .returnedLimit = 100,
                              .total = 0,
                              .queryState = "query-state",
                              .isAuthoritative = true,
                              .emailIds = {},
                          })
                          .has_value());
    }
} // namespace

TEST_CASE("synchronized mailbox coverage requires every canonical window to be authoritative",
          "[app][sync][coverage][pagination]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabaseContext();
    const std::vector<std::string> mailboxIds{"inbox", "archive"};

    auto coverage = javelin::app::hasAuthoritativeCanonicalMailboxCoverage(database.connection,
                                                                           "account-1", mailboxIds);
    REQUIRE(std::holds_alternative<bool>(coverage));
    CHECK_FALSE(std::get<bool>(coverage));

    storeWindow(database.connection, "inbox");
    coverage = javelin::app::hasAuthoritativeCanonicalMailboxCoverage(database.connection,
                                                                      "account-1", mailboxIds);
    REQUIRE(std::holds_alternative<bool>(coverage));
    CHECK_FALSE(std::get<bool>(coverage));

    storeWindow(database.connection, "archive");
    coverage = javelin::app::hasAuthoritativeCanonicalMailboxCoverage(database.connection,
                                                                      "account-1", mailboxIds);
    REQUIRE(std::holds_alternative<bool>(coverage));
    CHECK(std::get<bool>(coverage));

    javelin::jmap::cache::MailboxWindowRepository windows{database.connection};
    auto transaction = javelin::jmap::cache::DatabaseTransaction::begin(
        database.connection, QStringLiteral("Invalidate mailbox window"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transaction));
    auto activeTransaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transaction));
    REQUIRE_FALSE(windows.invalidateMailbox(activeTransaction, "account-1", "archive").has_value());
    REQUIRE_FALSE(activeTransaction.commit().has_value());

    coverage = javelin::app::hasAuthoritativeCanonicalMailboxCoverage(database.connection,
                                                                      "account-1", mailboxIds);
    REQUIRE(std::holds_alternative<bool>(coverage));
    CHECK_FALSE(std::get<bool>(coverage));
}
