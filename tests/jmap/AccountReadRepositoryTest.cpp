#include "jmap/cache/AccountReadRepository.h"

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
        return QStringLiteral("javelin-account-read-%1").arg(++counter);
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection, const QString& accountId,
                     const QString& name, const bool isPrimary)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, email_address, session_url, is_primary, name, "
            "is_personal, is_read_only, cap_mail, cap_submission, owner_account_id) VALUES ("
            ":account_id, :email_address, :session_url, :is_primary, :name, :is_personal, "
            ":is_read_only, :cap_mail, :cap_submission, :owner_account_id)"));
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
        query.bindValue(QStringLiteral(":owner_account_id"), accountId);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("account read repository uses a GUI read-only connection", "[jmap][cache]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    auto writerResult = javelin::jmap::cache::DaemonDatabaseFactory{
        javelin::jmap::cache::DatabaseConnectionOptions{
            .connectionName = connectionName(),
            .databasePath = databasePath,
        }}.open();
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(writerResult));
    auto writer = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(writerResult));
    seedAccount(writer, QStringLiteral("work"), QStringLiteral("Work"), false);
    seedAccount(writer, QStringLiteral("personal"), QStringLiteral("Personal"), true);
    writer = {};

    auto readerResult = javelin::jmap::cache::GuiDatabaseFactory{
        javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
            .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
            .databasePath = databasePath,
        }}.openForCurrentThread("accounts");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(readerResult));
    auto readerConnection =
        std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(readerResult));

    const javelin::jmap::cache::AccountReadRepository repository{readerConnection};
    const auto result = repository.listAll();
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::CachedAccount>>(result));
    const auto& accounts = std::get<std::vector<javelin::jmap::cache::CachedAccount>>(result);
    REQUIRE(accounts.size() == 2);
    CHECK(accounts.front().accountId == "personal");
    CHECK(accounts.front().isPrimary);
    CHECK(accounts.back().accountId == "work");
}
