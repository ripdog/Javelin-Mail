#include "app/MailNotificationService.h"

#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/domain/MailEntities.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoreApplication>
#include <QEventLoop>
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
                return;
            static int argc = 1;
            static char appName[] = "mail-notification-service-test";
            static char* argv[] = {appName, nullptr};
            m_application = std::make_unique<QCoreApplication>(argc, argv);
        }

      private:
        std::unique_ptr<QCoreApplication> m_application;
    };

    struct TestDatabase
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection connection;
    };

    [[nodiscard]] TestDatabase makeDatabase()
    {
        static int counter = 0;
        TestDatabase database;
        REQUIRE(database.directory.isValid());
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("mail-notification-service-%1").arg(++counter),
            .databasePath = database.directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
            FAIL(error->message.toStdString());
        database.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        return database;
    }

    [[nodiscard]] javelin::jmap::domain::Email pendingEmail()
    {
        return {
            .id = "email-1",
            .blobId = "blob-1",
            .threadId = "thread-1",
            .mailboxIds = {"inbox"},
            .keywords = {},
            .size = 42,
            .receivedAt = "2026-08-28T00:00:00Z",
            .sentAt = std::nullopt,
            .messageId = {},
            .inReplyTo = {},
            .references = {},
            .hasAttachment = false,
            .subject = "Pending message",
            .from = {},
            .to = {},
            .cc = {},
            .bcc = {},
            .replyTo = {},
            .preview = std::nullopt,
        };
    }

    void seedPendingNotification(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery seed{connection.database()};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary,cap_mail) "
            "VALUES('account-1','user@example.test','https://example.test/jmap',1,1)")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO mailboxes(account_id,mailbox_id,name,role,total_emails,total_threads,"
            "is_subscribed) VALUES('account-1','inbox','Inbox','inbox',1,1,1)")));

        auto email = pendingEmail();
        javelin::jmap::cache::EmailRepository emails{connection};
        REQUIRE_FALSE(emails.upsertMany("account-1", {email}).has_value());

        javelin::jmap::cache::NotificationRepository notifications{connection};
        REQUIRE_FALSE(notifications
                          .synchronizeMailboxHorizons("account-1", {"inbox"},
                                                      std::string_view{"email-state-1"})
                          .has_value());
        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            connection, QStringLiteral("Seed mail notification"));
        REQUIRE(
            std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
        auto transaction =
            std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
        const auto created =
            notifications.createEventIfUnconsumed(transaction, "account-1",
                                                  {.mailboxId = "inbox",
                                                   .emailId = email.id,
                                                   .threadId = email.threadId,
                                                   .subject = email.subject,
                                                   .receivedAt = email.receivedAt});
        REQUIRE(std::holds_alternative<bool>(created));
        REQUIRE(std::get<bool>(created));
        REQUIRE_FALSE(transaction.commit().has_value());
    }

    void claimPendingNotification(javelin::jmap::cache::DatabaseConnection& connection)
    {
        javelin::jmap::cache::NotificationRepository notifications{connection};
        const auto claimed = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                claimed));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(claimed)
                  .size() == 1);
    }

    [[nodiscard]] int rowCount(javelin::jmap::cache::DatabaseConnection& connection,
                               const QString& table)
    {
        QSqlQuery query{connection.database()};
        REQUIRE(query.exec(QStringLiteral("SELECT COUNT(*) FROM %1").arg(table)));
        REQUIRE(query.next());
        return query.value(0).toInt();
    }

    void processImmediateRetry()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }
} // namespace

TEST_CASE("mail notification claim failure requests a later delivery pass",
          "[app][notification][mail][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedPendingNotification(database.connection);

    QSqlQuery failClaim{database.connection.database()};
    REQUIRE(failClaim.exec(QStringLiteral(
        "CREATE TRIGGER fail_mail_notification_claim BEFORE INSERT ON notification_dispatch_claims "
        "WHEN NEW.kind='mail' BEGIN SELECT RAISE(FAIL,'forced claim failure'); END")));

    javelin::app::MailNotificationService service{database.connection};
    QStringList retryAccounts;
    QObject::connect(&service, &javelin::app::MailNotificationService::deliveryRetryRequired,
                     [&retryAccounts](const QString& accountId)
                     { retryAccounts.push_back(accountId); });

    service.accountChanged(QStringLiteral("account-1"));

    CHECK(retryAccounts == QStringList{QStringLiteral("account-1")});
    CHECK(rowCount(database.connection, QStringLiteral("mail_notification_event_outbox")) == 1);
    CHECK(rowCount(database.connection, QStringLiteral("notification_dispatch_claims")) == 0);
}

TEST_CASE("mail notification acknowledgement failure retries without redelivery",
          "[app][notification][mail][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedPendingNotification(database.connection);
    claimPendingNotification(database.connection);

    QSqlQuery failDelivery{database.connection.database()};
    REQUIRE(failDelivery.exec(QStringLiteral(
        "CREATE TRIGGER fail_mail_notification_delivery BEFORE DELETE ON "
        "mail_notification_event_outbox BEGIN SELECT RAISE(FAIL,'forced delivery failure'); END")));

    javelin::app::MailNotificationService service{database.connection};
    int raisedCount = 0;
    QObject::connect(&service, &javelin::app::MailNotificationService::notificationRaised,
                     [&raisedCount](const QString&, const QString&, const QString&, const QString&,
                                    const QString&, const QString&, const QString&,
                                    const QStringList&) { ++raisedCount; });

    const auto failed = service.markDelivered("account-1", {QStringLiteral("email-1")});
    REQUIRE(failed.has_value());
    REQUIRE(failDelivery.exec(QStringLiteral("DROP TRIGGER fail_mail_notification_delivery")));

    processImmediateRetry();

    CHECK(raisedCount == 0);
    CHECK(rowCount(database.connection, QStringLiteral("mail_notification_event_outbox")) == 0);
    CHECK(rowCount(database.connection, QStringLiteral("notification_dispatch_claims")) == 0);
}

TEST_CASE("mail notification release failure retries before rearming delivery",
          "[app][notification][mail][retry]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto database = makeDatabase();
    seedPendingNotification(database.connection);
    claimPendingNotification(database.connection);

    QSqlQuery failRelease{database.connection.database()};
    REQUIRE(failRelease.exec(
        QStringLiteral("CREATE TRIGGER fail_mail_notification_release BEFORE DELETE ON "
                       "notification_dispatch_claims WHEN OLD.kind='mail' "
                       "BEGIN SELECT RAISE(FAIL,'forced release failure'); END")));

    javelin::app::MailNotificationService service{database.connection};
    QStringList retryAccounts;
    QObject::connect(&service, &javelin::app::MailNotificationService::deliveryRetryRequired,
                     [&retryAccounts](const QString& accountId)
                     { retryAccounts.push_back(accountId); });

    const auto failed = service.releaseDispatches("account-1", {QStringLiteral("email-1")});
    REQUIRE(failed.has_value());
    REQUIRE(failRelease.exec(QStringLiteral("DROP TRIGGER fail_mail_notification_release")));

    processImmediateRetry();

    CHECK(retryAccounts == QStringList{QStringLiteral("account-1")});
    CHECK(rowCount(database.connection, QStringLiteral("mail_notification_event_outbox")) == 1);
    CHECK(rowCount(database.connection, QStringLiteral("notification_dispatch_claims")) == 0);
}
