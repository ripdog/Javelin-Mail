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
            .databasePath = context.temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
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
        query.prepare(
            QStringLiteral("INSERT INTO accounts ("
                           "account_id, email_address, session_url, is_primary, name, is_personal, "
                           "is_read_only, cap_mail, cap_submission"
                           ") VALUES ("
                           ":account_id, :email_address, :session_url, :is_primary, :name, "
                           ":is_personal, :is_read_only, :cap_mail, :cap_submission)"));
        query.bindValue(QStringLiteral(":account_id"), accountId);
        query.bindValue(QStringLiteral(":email_address"),
                        QStringLiteral("%1@example.com").arg(accountId));
        query.bindValue(QStringLiteral(":session_url"),
                        QStringLiteral("https://mail.example.com/.well-known/jmap"));
        query.bindValue(QStringLiteral(":is_primary"), isPrimary ? 1 : 0);
        query.bindValue(QStringLiteral(":name"), name);
        query.bindValue(QStringLiteral(":is_personal"), 1);
        query.bindValue(QStringLiteral(":is_read_only"), 0);
        query.bindValue(QStringLiteral(":cap_mail"), 1);
        query.bindValue(QStringLiteral(":cap_submission"), 0);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("account repository lists cached accounts with primary accounts first", "[jmap][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection, QStringLiteral("account-2"), QStringLiteral("Work"),
                false);
    seedAccount(databaseContext.connection, QStringLiteral("account-1"), QStringLiteral("Personal"),
                true);

    javelin::jmap::cache::AccountRepository repository{databaseContext.connection};
    const auto result = repository.listAll();

    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::CachedAccount>>(result));
    const auto& accounts = std::get<std::vector<javelin::jmap::cache::CachedAccount>>(result);
    REQUIRE(accounts.size() == 2);
    CHECK(accounts.front().accountId == "account-1");
    CHECK(accounts.front().isPrimary);
    CHECK(accounts.back().accountId == "account-2");
}

TEST_CASE("removing cached accounts cascades all account-owned data", "[jmap][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection, QStringLiteral("account-1"), QStringLiteral("Personal"),
                true);

    QSqlQuery mailbox{databaseContext.connection.database()};
    mailbox.prepare(QStringLiteral(
        "INSERT INTO mailboxes (account_id, mailbox_id, name) VALUES (:account_id, :mailbox_id, "
        ":name)"));
    mailbox.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
    mailbox.bindValue(QStringLiteral(":mailbox_id"), QStringLiteral("inbox"));
    mailbox.bindValue(QStringLiteral(":name"), QStringLiteral("Inbox"));
    REQUIRE(mailbox.exec());

    javelin::jmap::cache::AccountRepository repository{databaseContext.connection};
    REQUIRE_FALSE(repository.removeMany({QStringLiteral("account-1")}).has_value());

    QSqlQuery accountCount{databaseContext.connection.database()};
    REQUIRE(accountCount.exec(QStringLiteral("SELECT COUNT(*) FROM accounts")));
    REQUIRE(accountCount.next());
    CHECK(accountCount.value(0).toInt() == 0);

    QSqlQuery mailboxCount{databaseContext.connection.database()};
    REQUIRE(mailboxCount.exec(QStringLiteral("SELECT COUNT(*) FROM mailboxes")));
    REQUIRE(mailboxCount.next());
    CHECK(mailboxCount.value(0).toInt() == 0);
}
