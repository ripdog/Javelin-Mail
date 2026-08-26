#include "FixtureReader.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/NotificationRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/sync/MailNotificationEligibility.h"

#include <QCoreApplication>
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
        return QStringLiteral("javelin-notification-planner-%1").arg(counter);
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

    void seedAccount(javelin::jmap::cache::DatabaseConnection& connection)
    {
        QSqlQuery query{connection.database()};
        query.prepare(QStringLiteral(
            "INSERT INTO accounts (account_id, email_address, session_url, is_primary) "
            "VALUES (:account_id, :email_address, :session_url, :is_primary)"));
        query.bindValue(QStringLiteral(":account_id"), QStringLiteral("account-1"));
        query.bindValue(QStringLiteral(":email_address"), QStringLiteral("alice@example.com"));
        query.bindValue(QStringLiteral(":session_url"),
                        QStringLiteral("https://mail.example.com/.well-known/jmap"));
        query.bindValue(QStringLiteral(":is_primary"), 1);
        REQUIRE(query.exec());
    }

    [[nodiscard]] javelin::jmap::domain::Email loadEmailFixture()
    {
        const auto parsed = javelin::jmap::domain::parseEmail(
            javelin::tests::loadFixture("jmap/entities/email.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

} // namespace

TEST_CASE("mail notification eligibility requires a legitimate incoming transition",
          "[jmap][sync][notification][eligibility]")
{
    auto previous = loadEmailFixture();
    previous.id = "eml-transition";
    previous.threadId = "thr-transition";
    previous.mailboxIds = {"mbx-archive"};
    previous.keywords = {};

    auto current = previous;
    const std::vector<std::string> enabled{"mbx-inbox", "mbx-projects"};
    const auto evaluate = [&](const javelin::jmap::domain::Email* before, const bool serverCreated,
                              const bool withinHorizon, const bool suppressed)
    {
        return javelin::jmap::sync::evaluateMailNotificationTransition({
            .previous = before,
            .current = &current,
            .notificationMailboxIds = enabled,
            .serverCreated = serverCreated,
            .withinNotificationHorizon = withinHorizon,
            .suppressedByLocalOperation = suppressed,
        });
    };

    SECTION("new unread mail in one enabled mailbox")
    {
        current.mailboxIds = {"mbx-inbox"};
        const auto decision = evaluate(nullptr, true, true, false);
        CHECK(decision.qualifyingMailboxIds == std::vector<std::string>{"mbx-inbox"});
    }

    SECTION("new unread mail in several enabled mailboxes is still one Email event")
    {
        current.mailboxIds = {"mbx-projects", "mbx-inbox"};
        const auto decision = evaluate(nullptr, true, true, false);
        CHECK(decision.qualifyingMailboxIds ==
              std::vector<std::string>{"mbx-inbox", "mbx-projects"});
    }

    SECTION("new mail already read is not eligible")
    {
        current.mailboxIds = {"mbx-inbox"};
        current.keywords = {"$seen"};
        CHECK_FALSE(evaluate(nullptr, true, true, false).eligible());
    }

    SECTION("server-side unread move into an enabled mailbox is eligible")
    {
        current.mailboxIds = {"mbx-inbox"};
        const auto decision = evaluate(&previous, false, true, false);
        CHECK(decision.qualifyingMailboxIds == std::vector<std::string>{"mbx-inbox"});
    }

    SECTION("move between enabled mailboxes is not another incoming transition")
    {
        previous.mailboxIds = {"mbx-inbox"};
        current.mailboxIds = {"mbx-projects"};
        CHECK_FALSE(evaluate(&previous, false, true, false).eligible());
    }

    SECTION("seen to unread in an already enabled mailbox is not arrival")
    {
        previous.mailboxIds = {"mbx-inbox"};
        previous.keywords = {"$seen"};
        current.mailboxIds = {"mbx-inbox"};
        current.keywords = {};
        CHECK_FALSE(evaluate(&previous, false, true, false).eligible());
    }

    SECTION("uncached updated mail is not guessed to be newly eligible")
    {
        current.mailboxIds = {"mbx-inbox"};
        CHECK_FALSE(evaluate(nullptr, false, true, false).eligible());
    }

    SECTION("pre-horizon transition is historical")
    {
        current.mailboxIds = {"mbx-inbox"};
        CHECK_FALSE(evaluate(nullptr, true, false, false).eligible());
    }

    SECTION("local import provenance suppresses a server-created Email")
    {
        current.mailboxIds = {"mbx-inbox"};
        CHECK_FALSE(evaluate(nullptr, true, true, true).eligible());
    }
}

TEST_CASE("notification delivery revalidates pending events locally",
          "[jmap][cache][notification][delivery]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto email = loadEmailFixture();
    email.id = "eml-pending";
    email.threadId = "thr-pending";
    email.mailboxIds = {"mbx-inbox"};
    email.keywords = {};
    email.subject = "Pending message";

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {email}).has_value());
    javelin::jmap::cache::NotificationRepository notifications{databaseContext.connection};
    REQUIRE_FALSE(notifications
                      .synchronizeMailboxHorizons("account-1", {"mbx-inbox"},
                                                  std::string_view{"email-state-1"})
                      .has_value());

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Create pending notification"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    const auto created = notifications.createEventIfUnconsumed(transaction, "account-1",
                                                               {.mailboxId = "mbx-inbox",
                                                                .emailId = email.id,
                                                                .threadId = email.threadId,
                                                                .subject = email.subject,
                                                                .receivedAt = email.receivedAt});
    REQUIRE(std::holds_alternative<bool>(created));
    REQUIRE(std::get<bool>(created));
    REQUIRE_FALSE(transaction.commit().has_value());

    const auto consumptionCount = [&]()
    {
        QSqlQuery query{databaseContext.connection.database()};
        REQUIRE(query.exec(QStringLiteral(
            "SELECT COUNT(*) FROM mail_notification_state WHERE account_id='account-1' AND "
            "email_id='eml-pending'")));
        REQUIRE(query.next());
        return query.value(0).toInt();
    };

    SECTION("successful delivery deletes only the outbox row")
    {
        const auto claimed = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                claimed));
        REQUIRE(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(claimed)
                    .size() == 1);
        REQUIRE_FALSE(notifications.markDelivered("account-1", {email.id}).has_value());
        const auto pending = notifications.listPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                pending));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pending)
                  .empty());
        CHECK(consumptionCount() == 1);
    }

    SECTION("read before retry cancels the stale popup")
    {
        const auto claimed = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                claimed));
        REQUIRE_FALSE(notifications.releaseDispatches("account-1", {email.id}).has_value());
        email.keywords = {"$seen"};
        REQUIRE_FALSE(emails.upsertMany("account-1", {email}).has_value());
        const auto retried = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                retried));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(retried)
                  .empty());
        CHECK(consumptionCount() == 1);
    }

    SECTION("move out before retry cancels the stale popup")
    {
        const auto claimed = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                claimed));
        REQUIRE_FALSE(notifications.releaseDispatches("account-1", {email.id}).has_value());
        const std::vector<std::string> ids{email.id};
        REQUIRE_FALSE(emails.removeFromMailbox("account-1", "mbx-inbox", ids).has_value());
        const auto retried = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                retried));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(retried)
                  .empty());
        CHECK(consumptionCount() == 1);
    }

    SECTION("disabled notification context cancels the stale popup")
    {
        REQUIRE_FALSE(
            notifications
                .synchronizeMailboxHorizons("account-1", {}, std::string_view{"email-state-1"})
                .has_value());
        const auto claimed = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                claimed));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(claimed)
                  .empty());
        CHECK(consumptionCount() == 1);
    }

    SECTION("destroyed Email has no pending popup")
    {
        const std::vector<std::string> ids{email.id};
        REQUIRE_FALSE(emails.removeMany("account-1", ids).has_value());
        const auto claimed = notifications.claimPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                claimed));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(claimed)
                  .empty());
        CHECK(consumptionCount() == 0);
    }

    SECTION("local cancellation failure leaves the event retryable")
    {
        email.keywords = {"$seen"};
        REQUIRE_FALSE(emails.upsertMany("account-1", {email}).has_value());
        QSqlQuery failDelete{databaseContext.connection.database()};
        REQUIRE(failDelete.exec(
            QStringLiteral("CREATE TRIGGER fail_pending_notification_delete BEFORE DELETE ON "
                           "mail_notification_event_outbox BEGIN SELECT RAISE(FAIL,'forced local "
                           "failure'); END")));
        const auto failed = notifications.claimPendingEvents("account-1");
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseError>(failed));
        const auto pending = notifications.listPendingEvents("account-1");
        REQUIRE(
            std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
                pending));
        CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(pending)
                  .size() == 1);
        CHECK(consumptionCount() == 1);
    }
}

TEST_CASE("per-Email notification consumption survives delivery and mailbox movement",
          "[jmap][cache][notification][consumption]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto email = loadEmailFixture();
    email.id = "eml-one-shot";
    email.threadId = "thr-one-shot";
    email.mailboxIds = {"mbx-inbox", "mbx-archive"};
    email.keywords = {};
    email.subject = "One shot";

    javelin::jmap::cache::EmailRepository emails{databaseContext.connection};
    REQUIRE_FALSE(emails.replaceAll("account-1", {email}).has_value());
    javelin::jmap::cache::NotificationRepository notifications{databaseContext.connection};

    auto firstTransactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Create first notification event"));
    REQUIRE(
        std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(firstTransactionResult));
    auto firstTransaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(firstTransactionResult));
    const auto first = notifications.createEventIfUnconsumed(firstTransaction, "account-1",
                                                             {
                                                                 .mailboxId = "mbx-inbox",
                                                                 .emailId = email.id,
                                                                 .threadId = email.threadId,
                                                                 .subject = email.subject,
                                                                 .receivedAt = email.receivedAt,
                                                             });
    REQUIRE(std::holds_alternative<bool>(first));
    CHECK(std::get<bool>(first));
    REQUIRE_FALSE(firstTransaction.commit().has_value());

    const auto firstPending = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        firstPending));
    const auto& pendingEvents =
        std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(firstPending);
    REQUIRE(pendingEvents.size() == 1);
    CHECK(pendingEvents.front().emailId == email.id);
    CHECK(pendingEvents.front().mailboxId == "mbx-inbox");

    QSqlQuery delivered{databaseContext.connection.database()};
    REQUIRE(delivered.exec(QStringLiteral(
        "DELETE FROM mail_notification_event_outbox WHERE account_id='account-1' AND "
        "email_id='eml-one-shot'")));

    auto secondTransactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Reject duplicate notification event"));
    REQUIRE(
        std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(secondTransactionResult));
    auto secondTransaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(secondTransactionResult));
    const auto second = notifications.createEventIfUnconsumed(secondTransaction, "account-1",
                                                              {
                                                                  .mailboxId = "mbx-archive",
                                                                  .emailId = email.id,
                                                                  .threadId = email.threadId,
                                                                  .subject = email.subject,
                                                                  .receivedAt = email.receivedAt,
                                                              });
    REQUIRE(std::holds_alternative<bool>(second));
    CHECK_FALSE(std::get<bool>(second));
    REQUIRE_FALSE(secondTransaction.commit().has_value());

    const auto afterDelivery = notifications.listPendingEvents("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(
        afterDelivery));
    CHECK(std::get<std::vector<javelin::jmap::cache::MailNotificationPendingEvent>>(afterDelivery)
              .empty());

    const std::vector<std::string> ids{email.id};
    REQUIRE_FALSE(emails.removeFromMailbox("account-1", "mbx-inbox", ids).has_value());
    QSqlQuery retainedState{databaseContext.connection.database()};
    REQUIRE(retainedState.exec(QStringLiteral(
        "SELECT COUNT(*) FROM mail_notification_state WHERE account_id='account-1' AND "
        "email_id='eml-one-shot'")));
    REQUIRE(retainedState.next());
    CHECK(retainedState.value(0).toInt() == 1);

    REQUIRE_FALSE(emails.removeMany("account-1", ids).has_value());
    QSqlQuery removedState{databaseContext.connection.database()};
    REQUIRE(removedState.exec(QStringLiteral(
        "SELECT COUNT(*) FROM mail_notification_state WHERE account_id='account-1' AND "
        "email_id='eml-one-shot'")));
    REQUIRE(removedState.next());
    CHECK(removedState.value(0).toInt() == 0);
}

TEST_CASE("notification mailbox horizons linearize enablement against Email state",
          "[jmap][cache][notification][horizon]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);
    javelin::jmap::cache::NotificationRepository notifications{databaseContext.connection};

    REQUIRE_FALSE(notifications
                      .synchronizeMailboxHorizons("account-1", {"mbx-inbox", "mbx-archive"},
                                                  std::string_view{"email-state-1"})
                      .has_value());
    const auto initial = notifications.mailboxHorizonsAtState("account-1", "email-state-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(initial));
    CHECK(std::get<std::vector<std::string>>(initial) ==
          std::vector<std::string>{"mbx-archive", "mbx-inbox"});

    REQUIRE_FALSE(notifications
                      .synchronizeMailboxHorizons("account-1", {"mbx-archive"},
                                                  std::string_view{"email-state-1"})
                      .has_value());
    const auto afterDisable = notifications.mailboxHorizonsAtState("account-1", "email-state-1");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(afterDisable));
    CHECK(std::get<std::vector<std::string>>(afterDisable) ==
          std::vector<std::string>{"mbx-archive"});

    auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
        databaseContext.connection, QStringLiteral("Advance notification horizon"));
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(transactionResult));
    auto transaction =
        std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
    REQUIRE_FALSE(
        notifications
            .advanceMailboxHorizons(transaction, "account-1", "email-state-1", "email-state-2")
            .has_value());
    REQUIRE_FALSE(transaction.commit().has_value());

    const auto advanced = notifications.mailboxHorizonsAtState("account-1", "email-state-2");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(advanced));
    CHECK(std::get<std::vector<std::string>>(advanced) == std::vector<std::string>{"mbx-archive"});

    REQUIRE_FALSE(notifications
                      .synchronizeMailboxHorizons("account-1", {"mbx-archive", "mbx-projects"},
                                                  std::string_view{"email-state-2"})
                      .has_value());
    const auto afterEnable = notifications.mailboxHorizonsAtState("account-1", "email-state-2");
    REQUIRE(std::holds_alternative<std::vector<std::string>>(afterEnable));
    CHECK(std::get<std::vector<std::string>>(afterEnable) ==
          std::vector<std::string>{"mbx-archive", "mbx-projects"});
}
