#include "jmap/cache/MailboxReadRepository.h"

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
        return QStringLiteral("javelin-mailbox-read-%1").arg(++counter);
    }

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, email_address, session_url, is_primary, name, "
            "is_personal, is_read_only, cap_mail, cap_submission, owner_account_id) VALUES ("
            ":account_id, :email_address, :session_url, :is_primary, :name, :is_personal, "
            ":is_read_only, :cap_mail, :cap_submission, :owner_account_id)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        query.bindValue(QStringLiteral(":email_address"), QStringLiteral("account@example.com"));
        query.bindValue(QStringLiteral(":session_url"),
                        QStringLiteral("https://mail.example.com/.well-known/jmap"));
        query.bindValue(QStringLiteral(":is_primary"), 1);
        query.bindValue(QStringLiteral(":name"), QStringLiteral("Account"));
        query.bindValue(QStringLiteral(":is_personal"), 1);
        query.bindValue(QStringLiteral(":is_read_only"), 0);
        query.bindValue(QStringLiteral(":cap_mail"), 1);
        query.bindValue(QStringLiteral(":cap_submission"), 0);
        query.bindValue(QStringLiteral(":owner_account_id"), QStringLiteral("account-1"));
        REQUIRE(query.exec());
    }

    void seedMailbox(javelin::jmap::cache::DatabaseConnection& connection, const QString& mailboxId,
                     const QVariant& parentMailboxId, const QString& name, const QString& role,
                     const int sortOrder, const bool subscribed, const QString& rightsJson)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO mailboxes ("
            "account_id, mailbox_id, parent_mailbox_id, name, role, sort_order, total_emails, "
            "unread_emails, total_threads, unread_threads, is_subscribed, rights_json, state"
            ") VALUES ("
            ":account_id, :mailbox_id, :parent_mailbox_id, :name, :role, :sort_order, "
            ":total_emails, :unread_emails, :total_threads, :unread_threads, :is_subscribed, "
            ":rights_json, :state)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        query.bindValue(QStringLiteral(":mailbox_id"), mailboxId);
        query.bindValue(QStringLiteral(":parent_mailbox_id"), parentMailboxId);
        query.bindValue(QStringLiteral(":name"), name);
        query.bindValue(QStringLiteral(":role"), role);
        query.bindValue(QStringLiteral(":sort_order"), sortOrder);
        query.bindValue(QStringLiteral(":total_emails"), 3);
        query.bindValue(QStringLiteral(":unread_emails"), 2);
        query.bindValue(QStringLiteral(":total_threads"), 2);
        query.bindValue(QStringLiteral(":unread_threads"), 1);
        query.bindValue(QStringLiteral(":is_subscribed"), subscribed ? 1 : 0);
        query.bindValue(QStringLiteral(":rights_json"), rightsJson);
        query.bindValue(QStringLiteral(":state"), QStringLiteral("state"));
        REQUIRE(query.exec());
    }

} // namespace

TEST_CASE("mailbox read repository uses a GUI read-only connection", "[jmap][cache]")
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
    seedAccount(writer);
    seedMailbox(writer, QStringLiteral("inbox"), QVariant{}, QStringLiteral("Inbox"),
                QStringLiteral("inbox"), 10, true, QStringLiteral("{\"mayReadItems\":true}"));
    seedMailbox(writer, QStringLiteral("archive"), QStringLiteral("inbox"),
                QStringLiteral("Archive"), QString{}, 20, false, QStringLiteral("{}"));
    writer = {};

    auto readerResult = javelin::jmap::cache::GuiDatabaseFactory{
        javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
            .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
            .databasePath = databasePath,
        }}.openForCurrentThread("mailboxes");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(readerResult));
    auto readerConnection =
        std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(readerResult));

    const javelin::jmap::cache::MailboxReadRepository repository{readerConnection};
    const auto result = repository.listMailboxTree("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailboxTreeItem>>(result));
    const auto& mailboxes = std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(result);
    REQUIRE(mailboxes.size() == 2);
    CHECK(mailboxes.front().id == "inbox");
    CHECK(mailboxes.front().role == std::optional<std::string>{"inbox"});
    CHECK(mailboxes.front().isSubscribed);
    CHECK(mailboxes.front().myRights.mayReadItems);
    CHECK(mailboxes.front().hasChildren);
    CHECK(mailboxes.back().id == "archive");
    CHECK(mailboxes.back().parentId == std::optional<std::string>{"inbox"});
    CHECK_FALSE(mailboxes.back().isSubscribed);
}
