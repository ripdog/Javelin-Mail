#include "jmap/cache/AccountRepository.h"

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
        return QStringLiteral("javelin-account-%1").arg(counter);
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

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection, const QString& accountId,
                     const QString& name, const bool isPrimary)
    {
        QSqlQuery query{connection.database()};
        query.prepare("INSERT INTO accounts ("
                      "account_id, email_address, session_url, is_primary, name, is_personal, "
                      "is_read_only, cap_mail, cap_submission"
                      ") VALUES ("
                      ":account_id, :email_address, :session_url, :is_primary, :name, "
                      ":is_personal, :is_read_only, :cap_mail, :cap_submission)");
        query.bindValue(":account_id", accountId);
        query.bindValue(":email_address", accountId + "@example.com");
        query.bindValue(":session_url", "https://mail.example.com/.well-known/jmap");
        query.bindValue(":is_primary", isPrimary ? 1 : 0);
        query.bindValue(":name", name);
        query.bindValue(":is_personal", 1);
        query.bindValue(":is_read_only", 0);
        query.bindValue(":cap_mail", 1);
        query.bindValue(":cap_submission", 0);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("account repository lists cached accounts with primary accounts first", "[jmap][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection, "account-2", "Work", false);
    seedAccount(databaseContext.connection, "account-1", "Personal", true);

    javelin::jmap::cache::AccountRepository repository{databaseContext.connection};
    const auto result = repository.listAll();

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::CachedAccount>>(result));
    const auto& accounts = std::get<std::vector<javelin::jmap::cache::CachedAccount>>(result);
    REQUIRE(accounts.size() == 2);
    CHECK(accounts.front().accountId == "account-1");
    CHECK(accounts.front().isPrimary);
    CHECK(accounts.back().accountId == "account-2");
}
