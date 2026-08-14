#include "FixtureReader.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/domain/MailEntityParsers.h"

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
            static char appName[] = "javelin-read-tests";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    [[nodiscard]] QString connectionName()
    {
        static int counter = 0;
        return QStringLiteral("javelin-read-surface-%1").arg(++counter);
    }
} // namespace

TEST_CASE("read-only cache repositories expose cache reads", "[jmap][cache][read-only]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const auto databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));

    auto daemonResult =
        javelin::jmap::cache::DaemonDatabaseFactory{
            javelin::jmap::cache::DatabaseConnectionOptions{.connectionName = connectionName(),
                                                            .databasePath = databasePath}}
            .open();
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(daemonResult));
    auto daemon = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(daemonResult));
    QSqlQuery account{daemon.database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary) VALUES "
        "('account-1','alice@example.com','https://mail.example/jmap',1)")));
    const auto parsed = javelin::jmap::domain::parseMailbox(
        javelin::tests::loadFixture("jmap/entities/mailbox.json"));
    REQUIRE(parsed.ok());
    REQUIRE(parsed.value.has_value());
    REQUIRE_FALSE(javelin::jmap::cache::MailboxRepository{daemon}
                      .replaceAll("account-1", {*parsed.value})
                      .has_value());

    auto guiResult =
        javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-read-surface"),
                .databasePath = databasePath}}
            .openForCurrentThread("query");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(guiResult));
    auto gui = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(guiResult));
    javelin::jmap::cache::MailboxReadRepository mailboxReader{gui};

    const auto tree = mailboxReader.listMailboxTree("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailboxTreeItem>>(tree));
    REQUIRE(std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(tree).size() == 1);
}

TEST_CASE("message view service can load through a read-only connection",
          "[jmap][cache][read-only]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const auto databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    auto daemonResult = javelin::jmap::cache::DatabaseConnection::open(
        {.connectionName = connectionName(), .databasePath = databasePath});
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(daemonResult));
    auto daemon = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(daemonResult));

    QSqlQuery account{daemon.database()};
    REQUIRE(account.exec(QStringLiteral(
        "INSERT INTO accounts(account_id,email_address,session_url,is_primary) VALUES "
        "('account-1','alice@example.com','https://mail.example/jmap',1)")));
    QSqlQuery email{daemon.database()};
    REQUIRE(email.exec(QStringLiteral(
        "INSERT INTO emails(account_id,email_id,thread_id,blob_id,received_at,subject,preview,"
        "mailbox_ids_json,keywords_json,has_attachment,size) VALUES "
        "('account-1','email-1','thread-1','blob-1','2026-01-01T00:00:00Z','Subject','Preview',"
        "'[]','{}',0,12)")));
    QSqlQuery source{daemon.database()};
    source.prepare(QStringLiteral(
        "INSERT INTO raw_message_sources(account_id,email_id,blob_id,payload) VALUES "
        "(:account,:email,:blob,:payload)"));
    source.bindValue(QStringLiteral(":account"), QStringLiteral("account-1"));
    source.bindValue(QStringLiteral(":email"), QStringLiteral("email-1"));
    source.bindValue(QStringLiteral(":blob"), QStringLiteral("blob-1"));
    source.bindValue(QStringLiteral(":payload"),
                     QByteArrayLiteral("Subject: Subject\r\n\r\nRead-only body\r\n"));
    REQUIRE(source.exec());

    auto guiResult =
        javelin::jmap::cache::GuiDatabaseFactory{
            javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
                .connectionNamePrefix = QStringLiteral("javelin-read-surface"),
                .databasePath = databasePath}}
            .openForCurrentThread("message-view");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(guiResult));
    auto gui = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(guiResult));
    const auto result = javelin::jmap::cache::MessageViewService{gui}.load("account-1", "email-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result));
    const auto& snapshot =
        std::get<std::optional<javelin::jmap::cache::MessageViewSnapshot>>(result);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->plainTextBody.has_value());
    CHECK(snapshot->plainTextBody->value == QStringLiteral("Read-only body"));
}
