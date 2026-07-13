#include "jmap/cache/SessionRepository.h"
#include "FixtureReader.h"
#include "jmap/api/SessionParser.h"

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
        return QStringLiteral("javelin-session-%1").arg(counter);
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

    [[nodiscard]] javelin::jmap::api::Session loadSessionFixture()
    {
        const auto parsed = javelin::jmap::api::parseSession(
            javelin::tests::loadFixture("jmap/session/basic_session.json"), {
                                                                                .mail = true,
                                                                                .submission = true,
                                                                            });
        REQUIRE(parsed.ok());
        REQUIRE(parsed.session.has_value());
        return *parsed.session;
    }

} // namespace

TEST_CASE("session repository round-trips cached session bootstrap", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository repository{databaseContext.connection};
    auto session = loadSessionFixture();
    session.capabilities.contacts = true;
    session.primaryAccounts.contactsAccountId = "u1";
    session.accounts.at("u1").accountCapabilities.contacts = javelin::jmap::api::ContactsCapability{
        .maxAddressBooksPerCard = 8,
        .mayCreateAddressBook = true,
    };
    session.capabilities.calendars = true;
    session.primaryAccounts.calendarsAccountId = "u1";
    session.accounts.at("u1").accountCapabilities.calendars =
        javelin::jmap::api::CalendarsCapability{
            .maxCalendarsPerEvent = 4,
            .minDateTime = "1900-01-01T00:00:00Z",
            .maxDateTime = "2100-01-01T00:00:00Z",
            .maxExpandedQueryDuration = "P1Y",
            .maxParticipantsPerEvent = 100,
            .mayCreateCalendar = false,
        };

    if (const auto error = repository.replace("u1", session))
    {
        FAIL(error->message.toStdString());
    }

    const auto result = repository.load("u1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::api::Session>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::api::Session>>(result).has_value());
    const auto& loaded = *std::get<std::optional<javelin::jmap::api::Session>>(result);
    CHECK(loaded.username == "alice@example.com");
    CHECK(loaded.apiUrl == session.apiUrl);
    CHECK(loaded.capabilities.core);
    CHECK(loaded.capabilities.mail);
    CHECK(loaded.capabilities.submission);
    CHECK(loaded.capabilities.contacts);
    CHECK(loaded.capabilities.calendars);
    REQUIRE(loaded.capabilities.coreDetails.has_value());
    CHECK(loaded.capabilities.coreDetails->maxCallsInRequest == 16);
    CHECK(loaded.primaryAccounts.mailAccountId == session.primaryAccounts.mailAccountId);
    CHECK(loaded.primaryAccounts.contactsAccountId == "u1");
    CHECK(loaded.primaryAccounts.calendarsAccountId == "u1");
    REQUIRE(loaded.accounts.contains("u1"));
    CHECK(loaded.accounts.at("u1").accountCapabilities.mail);
    REQUIRE(loaded.accounts.at("u1").accountCapabilities.contacts.has_value());
    CHECK(loaded.accounts.at("u1").accountCapabilities.contacts->maxAddressBooksPerCard == 8);
    CHECK(loaded.accounts.at("u1").accountCapabilities.contacts->mayCreateAddressBook);
    REQUIRE(loaded.accounts.at("u1").accountCapabilities.calendars.has_value());
    CHECK(loaded.accounts.at("u1").accountCapabilities.calendars->maxExpandedQueryDuration ==
          "P1Y");
}

TEST_CASE("session repository replacement updates cached session rows", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository repository{databaseContext.connection};
    auto session = loadSessionFixture();

    if (const auto error = repository.replace("u1", session))
    {
        FAIL(error->message.toStdString());
    }

    session.state = "session-state-2";
    session.username = "updated@example.com";
    session.accounts.at("u1").name = "Updated";
    session.capabilities.coreDetails->maxConcurrentRequests = 42;
    if (const auto error = repository.replace("u1", session))
    {
        FAIL(error->message.toStdString());
    }

    const auto result = repository.load("u1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::api::Session>>(result));
    REQUIRE(std::get<std::optional<javelin::jmap::api::Session>>(result).has_value());
    const auto& loaded = *std::get<std::optional<javelin::jmap::api::Session>>(result);
    CHECK(loaded.state == "session-state-2");
    CHECK(loaded.username == "updated@example.com");
    CHECK(loaded.accounts.at("u1").name == "Updated");
    REQUIRE(loaded.capabilities.coreDetails.has_value());
    CHECK(loaded.capabilities.coreDetails->maxConcurrentRequests == 42);
}

TEST_CASE("session repository replacement preserves cached mailboxes for retained accounts",
          "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository repository{databaseContext.connection};
    auto session = loadSessionFixture();

    if (const auto error = repository.replace("u1", session))
    {
        FAIL(error->message.toStdString());
    }

    QSqlQuery mailboxQuery{databaseContext.connection.database()};
    mailboxQuery.prepare(QStringLiteral(
        "INSERT INTO mailboxes ("
        "account_id, mailbox_id, parent_mailbox_id, name, role, sort_order, total_emails, "
        "unread_emails, total_threads, unread_threads, is_subscribed, rights_json, state"
        ") VALUES ("
        ":account_id, :mailbox_id, :parent_mailbox_id, :name, :role, :sort_order, "
        ":total_emails, :unread_emails, :total_threads, :unread_threads, :is_subscribed, "
        ":rights_json, :state)"));
    mailboxQuery.bindValue(QStringLiteral(":account_id"), QStringLiteral("u1"));
    mailboxQuery.bindValue(QStringLiteral(":mailbox_id"), QStringLiteral("mbx-inbox"));
    mailboxQuery.bindValue(QStringLiteral(":parent_mailbox_id"), QVariant{});
    mailboxQuery.bindValue(QStringLiteral(":name"), QStringLiteral("Inbox"));
    mailboxQuery.bindValue(QStringLiteral(":role"), QStringLiteral("inbox"));
    mailboxQuery.bindValue(QStringLiteral(":sort_order"), 10);
    mailboxQuery.bindValue(QStringLiteral(":total_emails"), 1);
    mailboxQuery.bindValue(QStringLiteral(":unread_emails"), 1);
    mailboxQuery.bindValue(QStringLiteral(":total_threads"), 1);
    mailboxQuery.bindValue(QStringLiteral(":unread_threads"), 1);
    mailboxQuery.bindValue(QStringLiteral(":is_subscribed"), 1);
    mailboxQuery.bindValue(QStringLiteral(":rights_json"), QStringLiteral("{}"));
    mailboxQuery.bindValue(QStringLiteral(":state"), QVariant{});
    REQUIRE(mailboxQuery.exec());

    session.state = "session-state-2";
    if (const auto error = repository.replace("u1", session))
    {
        FAIL(error->message.toStdString());
    }

    QSqlQuery countQuery{databaseContext.connection.database()};
    REQUIRE(
        countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM mailboxes WHERE account_id = 'u1'")));
    REQUIRE(countQuery.next());
    CHECK(countQuery.value(0).toInt() == 1);
}

TEST_CASE("session repository returns no value for missing owners", "[jmap][cache][repository]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository repository{databaseContext.connection};

    const auto result = repository.load("missing");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::api::Session>>(result));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::api::Session>>(result).has_value());
}
