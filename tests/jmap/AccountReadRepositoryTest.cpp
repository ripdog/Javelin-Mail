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
                     const QString& name, const bool isPrimary, const bool hasMailCapability = true,
                     const bool hasSubmissionCapability = false,
                     const quint64 maxDelayedSendSeconds = 0, const QString& ownerAccountId = {},
                     const QString& connectionId = {}, const QString& remoteAccountId = {})
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, connection_id, remote_account_id, email_address, "
            "session_url, is_primary, name, is_personal, is_read_only, cap_mail, cap_submission, "
            "submission_max_delayed_send, owner_account_id) VALUES (:account_id, :connection_id, "
            ":remote_account_id, :email_address, :session_url, :is_primary, :name, :is_personal, "
            ":is_read_only, :cap_mail, :cap_submission, :submission_max_delayed_send, "
            ":owner_account_id)"));
        query.bindValue(QStringLiteral(":account_id"), accountId);
        const QString effectiveOwner = ownerAccountId.isEmpty() ? accountId : ownerAccountId;
        query.bindValue(QStringLiteral(":connection_id"),
                        connectionId.isEmpty() ? effectiveOwner : connectionId);
        query.bindValue(QStringLiteral(":remote_account_id"),
                        remoteAccountId.isEmpty() ? accountId : remoteAccountId);
        query.bindValue(QStringLiteral(":email_address"),
                        QStringLiteral("%1@example.com").arg(accountId));
        query.bindValue(QStringLiteral(":session_url"),
                        QStringLiteral("https://mail.example.com/.well-known/jmap"));
        query.bindValue(QStringLiteral(":is_primary"), isPrimary ? 1 : 0);
        query.bindValue(QStringLiteral(":name"), name);
        query.bindValue(QStringLiteral(":is_personal"), 1);
        query.bindValue(QStringLiteral(":is_read_only"), 0);
        query.bindValue(QStringLiteral(":cap_mail"), hasMailCapability ? 1 : 0);
        query.bindValue(QStringLiteral(":cap_submission"), hasSubmissionCapability ? 1 : 0);
        query.bindValue(QStringLiteral(":submission_max_delayed_send"), maxDelayedSendSeconds);
        query.bindValue(QStringLiteral(":owner_account_id"), effectiveOwner);
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("account read repository returns cached account metadata", "[jmap][cache]")
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
    seedAccount(writer, QStringLiteral("directory"), QStringLiteral("Directory"), false, false,
                true, 44236800, QStringLiteral("personal"));
    QSqlQuery mailboxPermission{writer.database()};
    REQUIRE(mailboxPermission.exec(QStringLiteral(
        "UPDATE accounts SET mail_may_create_top_level_mailbox=1 WHERE account_id='personal'")));
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
    REQUIRE(accounts.size() == 3);
    CHECK(accounts.front().accountId == "personal");
    CHECK(accounts.front().connectionId == "personal");
    CHECK(accounts.front().remoteAccountId == "personal");
    CHECK(accounts.front().isPrimary);
    CHECK(accounts.front().hasMailCapability);
    CHECK(accounts.front().mayCreateTopLevelMailbox);
    CHECK(accounts.at(1).accountId == "directory");
    CHECK(accounts.at(1).connectionId == "personal");
    CHECK(accounts.at(1).remoteAccountId == "directory");
    CHECK_FALSE(accounts.at(1).hasMailCapability);
    CHECK(accounts.at(1).hasSubmissionCapability);
    CHECK(accounts.at(1).maxDelayedSendSeconds == 44236800U);
    CHECK(accounts.at(1).ownerAccountId == "personal");
    CHECK(accounts.back().accountId == "work");

    const auto personalResult = repository.findById("personal");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CachedAccount>>(personalResult));
    const auto& personal =
        std::get<std::optional<javelin::jmap::cache::CachedAccount>>(personalResult);
    REQUIRE(personal.has_value());
    CHECK(personal->name == "Personal");
    CHECK(personal->mayCreateTopLevelMailbox);

    const auto directoryResult = repository.findById("directory");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::CachedAccount>>(
        directoryResult));
    const auto& directory =
        std::get<std::optional<javelin::jmap::cache::CachedAccount>>(directoryResult);
    REQUIRE(directory.has_value());
    CHECK_FALSE(directory->hasMailCapability);
    CHECK(directory->hasSubmissionCapability);
    CHECK(directory->maxDelayedSendSeconds == 44236800U);
    CHECK(directory->ownerAccountId == "personal");

    const auto missingResult = repository.findById("missing");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CachedAccount>>(missingResult));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::CachedAccount>>(missingResult).has_value());
}

TEST_CASE("account read repository namespaces duplicate remote account ids by connection",
          "[jmap][cache][account-identity]")
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

    seedAccount(writer, QStringLiteral("local-account-a"), QStringLiteral("Server A"), true, true,
                false, 0, QStringLiteral("local-account-a"), QStringLiteral("connection-a"),
                QStringLiteral("u1"));
    seedAccount(writer, QStringLiteral("local-account-b"), QStringLiteral("Server B"), true, true,
                false, 0, QStringLiteral("local-account-b"), QStringLiteral("connection-b"),
                QStringLiteral("u1"));
    writer = {};

    auto readerResult = javelin::jmap::cache::GuiDatabaseFactory{
        javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
            .connectionNamePrefix = QStringLiteral("javelin-gui-read-collision"),
            .databasePath = databasePath,
        }}.openForCurrentThread("accounts-collision");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(readerResult));
    auto readerConnection =
        std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(readerResult));
    const javelin::jmap::cache::AccountReadRepository repository{readerConnection};

    const auto firstResult = repository.findByLocator({
        .connectionId = "connection-a",
        .remoteAccountId = "u1",
    });
    const auto secondResult = repository.findByLocator({
        .connectionId = "connection-b",
        .remoteAccountId = "u1",
    });
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CachedAccount>>(firstResult));
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::CachedAccount>>(secondResult));
    const auto& first = std::get<std::optional<javelin::jmap::cache::CachedAccount>>(firstResult);
    const auto& second = std::get<std::optional<javelin::jmap::cache::CachedAccount>>(secondResult);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->accountId == "local-account-a");
    CHECK(second->accountId == "local-account-b");
    CHECK(first->remoteAccountId == second->remoteAccountId);
    CHECK(first->connectionId != second->connectionId);
}
