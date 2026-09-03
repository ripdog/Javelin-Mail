#include "jmap/cache/SyncStateRepository.h"
#include "storage/migrations/MigrationRunner.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <QString>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
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
        return QStringLiteral("javelin-test-db-%1").arg(counter);
    }

    [[nodiscard]] QString pragmaValue(const QSqlDatabase& database, const QString& name)
    {
        QSqlQuery query{database};
        REQUIRE(query.exec(QStringLiteral("PRAGMA %1").arg(name)));
        REQUIRE(query.next());
        return query.value(0).toString();
    }

} // namespace

TEST_CASE("database connection creates the initial cache schema", "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    auto result = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });

    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
    {
        FAIL(error->message.toStdString());
    }
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));

    REQUIRE_FALSE(connection.validate().has_value());
    CHECK(connection.schemaVersion() ==
          javelin::jmap::cache::createDefaultMigrationRunner().latestVersion());

    const auto migrationsResult = connection.appliedMigrations();
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::AppliedMigration>>(
        migrationsResult));
    const auto& migrations =
        std::get<std::vector<javelin::jmap::cache::AppliedMigration>>(migrationsResult);
    const auto runner = javelin::jmap::cache::createDefaultMigrationRunner();
    CHECK(std::ranges::equal(
        migrations, runner.steps(), [](const auto& applied, const auto& configured)
        { return applied.version == configured.version && applied.name == configured.name; }));

    CHECK(pragmaValue(connection.database(), QStringLiteral("foreign_keys")) ==
          QStringLiteral("1"));
    CHECK(pragmaValue(connection.database(), QStringLiteral("journal_mode"))
              .compare(QStringLiteral("wal"), Qt::CaseInsensitive) == 0);
    CHECK(pragmaValue(connection.database(), QStringLiteral("busy_timeout")) ==
          QStringLiteral("5000"));

    QSqlQuery mailboxWindowEmailIndex{connection.database()};
    REQUIRE(mailboxWindowEmailIndex.exec(
        QStringLiteral("PRAGMA index_info(idx_mailbox_query_window_items_email)")));
    std::vector<QString> indexedColumns;
    while (mailboxWindowEmailIndex.next())
        indexedColumns.push_back(mailboxWindowEmailIndex.value(2).toString());
    CHECK(indexedColumns ==
          std::vector<QString>{QStringLiteral("account_id"), QStringLiteral("email_id")});
}

TEST_CASE("database migrations are repeatable when reopening an existing cache",
          "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    {
        auto firstOpen = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = makeConnectionName(),
            .databasePath = databasePath,
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&firstOpen))
        {
            FAIL(error->message.toStdString());
        }
    }

    auto secondOpen = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&secondOpen))
    {
        FAIL(error->message.toStdString());
    }
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(secondOpen));

    const auto migrationsResult = connection.appliedMigrations();
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::AppliedMigration>>(
        migrationsResult));
    const auto& migrations =
        std::get<std::vector<javelin::jmap::cache::AppliedMigration>>(migrationsResult);
    const auto runner = javelin::jmap::cache::createDefaultMigrationRunner();
    CHECK(std::ranges::equal(
        migrations, runner.steps(), [](const auto& applied, const auto& configured)
        { return applied.version == configured.version && applied.name == configured.name; }));
    CHECK(connection.schemaVersion() == runner.latestVersion());
}

TEST_CASE("participant identity state migration preserves calendar tokens and widens the domain",
          "[jmap][cache][database][calendar]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("legacy-calendar-state-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto participantIdentityState = std::ranges::find_if(
            currentRunner.steps(), [](const auto& step) { return step.version == 61; });
        REQUIRE(participantIdentityState != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     participantIdentityState};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                           "('account-1','Calendar','calendar-state-1'),"
                           "('account-1','CalendarEventNotification','notification-state-1')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery preserved{migrated.database()};
    REQUIRE(preserved.exec(QStringLiteral(
        "SELECT data_type,state FROM calendar_state_tokens WHERE account_id='account-1' "
        "ORDER BY data_type")));
    REQUIRE(preserved.next());
    CHECK(preserved.value(0).toString() == QStringLiteral("Calendar"));
    CHECK(preserved.value(1).toString() == QStringLiteral("calendar-state-1"));
    REQUIRE(preserved.next());
    CHECK(preserved.value(0).toString() == QStringLiteral("CalendarEventNotification"));
    CHECK(preserved.value(1).toString() == QStringLiteral("notification-state-1"));
    CHECK_FALSE(preserved.next());

    QSqlQuery widened{migrated.database()};
    REQUIRE(widened.exec(
        QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                       "('account-1','ParticipantIdentity','participant-state-1')")));
}

TEST_CASE("pending calendar invitation migration isolates snapshots from event cache",
          "[jmap][cache][database][calendar][migration]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("calendar-invitation-snapshot-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    const QString eventDocument = QStringLiteral(
        R"({"@type":"Event","id":"event-1","uid":"uid-1","calendarIds":{},"title":"Invite","start":"2099-01-01T10:00:00","duration":"PT1H","showWithoutTime":false})");
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto isolatedInvitationSnapshots = std::ranges::find_if(
            currentRunner.steps(), [](const auto& step) { return step.version == 69; });
        REQUIRE(isolatedInvitationSnapshots != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     isolatedInvitationSnapshots};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        seed.prepare(QStringLiteral(
            "INSERT INTO calendar_events(account_id,event_id,uid,title,document_json,state) "
            "VALUES('account-1','event-1','uid-1','Invite',:document,'event-state')"));
        seed.bindValue(QStringLiteral(":document"), eventDocument);
        REQUIRE(seed.exec());
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO calendar_pending_invitations(account_id,event_id,recurrence_id,"
            "self_participant_id) VALUES('account-1','event-1','','self')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery pending{migrated.database()};
    REQUIRE(pending.exec(
        QStringLiteral("SELECT event_document_json FROM calendar_pending_invitations WHERE "
                       "account_id='account-1' AND event_id='event-1' AND recurrence_id=''")));
    REQUIRE(pending.next());
    CHECK(pending.value(0).toString() == eventDocument);

    QSqlQuery removeEvent{migrated.database()};
    REQUIRE(removeEvent.exec(QStringLiteral(
        "DELETE FROM calendar_events WHERE account_id='account-1' AND event_id='event-1'")));
    QSqlQuery retained{migrated.database()};
    REQUIRE(retained.exec(QStringLiteral(
        "SELECT COUNT(*) FROM calendar_pending_invitations WHERE account_id='account-1' AND "
        "event_id='event-1'")));
    REQUIRE(retained.next());
    CHECK(retained.value(0).toInt() == 1);
}

TEST_CASE("calendar window event state migration leaves legacy materialization unknown",
          "[jmap][cache][database][calendar][migration]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("legacy-calendar-window-state-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto windowEventState = std::ranges::find_if(
            currentRunner.steps(), [](const auto& step) { return step.version == 70; });
        REQUIRE(windowEventState != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     windowEventState};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO calendar_state_tokens(account_id,data_type,state) VALUES "
                           "('account-1','CalendarEvent','event-state-current')")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO calendar_query_windows(account_id,range_start,range_end,"
            "display_time_zone,query_state) VALUES('account-1','2026-09-01T00:00:00',"
            "'2026-10-01T00:00:00','Pacific/Auckland','query-state-old')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery columns{migrated.database()};
    REQUIRE(columns.exec(QStringLiteral("PRAGMA table_info(calendar_query_windows)")));
    bool foundEventState = false;
    while (columns.next())
    {
        if (columns.value(1).toString() == QStringLiteral("event_state"))
        {
            foundEventState = true;
            CHECK(columns.value(3).toInt() == 0);
        }
    }
    CHECK(foundEventState);

    QSqlQuery window{migrated.database()};
    REQUIRE(window.exec(QStringLiteral(
        "SELECT event_state FROM calendar_query_windows WHERE account_id='account-1'")));
    REQUIRE(window.next());
    CHECK(window.value(0).isNull());
}

TEST_CASE("durable calendar alert push migration cascades queued alerts with their owner",
          "[jmap][cache][database][calendar][notification][migration]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("calendar-alert-push-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto durablePushQueue = std::ranges::find_if(
            currentRunner.steps(), [](const auto& step) { return step.version == 71; });
        REQUIRE(durablePushQueue != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     durablePushQueue};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                           "VALUES('owner','owner@example.test','https://example.test/jmap',1)")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery queue{migrated.database()};
    REQUIRE(queue.exec(QStringLiteral(
        "INSERT INTO calendar_pushed_alerts(push_key,owner_account_id,account_id,event_id,uid,"
        "recurrence_id,alert_id) VALUES('push-1','owner','calendar-account','event-1','uid-1',"
        "'2026-09-03T10:00:00','alert-1')")));
    REQUIRE(queue.exec(QStringLiteral(
        "SELECT owner_account_id,account_id,event_id,uid,recurrence_id,alert_id FROM "
        "calendar_pushed_alerts WHERE push_key='push-1'")));
    REQUIRE(queue.next());
    CHECK(queue.value(0).toString() == QStringLiteral("owner"));
    CHECK(queue.value(1).toString() == QStringLiteral("calendar-account"));
    CHECK(queue.value(2).toString() == QStringLiteral("event-1"));
    CHECK(queue.value(3).toString() == QStringLiteral("uid-1"));
    CHECK(queue.value(4).toString() == QStringLiteral("2026-09-03T10:00:00"));
    CHECK(queue.value(5).toString() == QStringLiteral("alert-1"));

    QSqlQuery removeOwner{migrated.database()};
    REQUIRE(removeOwner.exec(QStringLiteral("DELETE FROM accounts WHERE account_id='owner'")));
    QSqlQuery retained{migrated.database()};
    REQUIRE(retained.exec(QStringLiteral("SELECT COUNT(*) FROM calendar_pushed_alerts")));
    REQUIRE(retained.next());
    CHECK(retained.value(0).toInt() == 0);
}

TEST_CASE("calendar reminder horizon migration cascades retention with its owner",
          "[jmap][cache][database][calendar][notification][migration]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("calendar-reminder-horizon-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto reminderTiming = std::ranges::find_if(currentRunner.steps(), [](const auto& step)
                                                         { return step.version == 73; });
        REQUIRE(reminderTiming != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     reminderTiming};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
                           "VALUES('owner','owner@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO calendar_events(account_id,event_id,uid,title,document_json,state) VALUES "
            "('owner','event-1','uid-1','Reminder',"
            "'{\"@type\":\"Event\",\"id\":\"event-1\",\"uid\":\"uid-1\","
            "\"calendarIds\":{},\"title\":\"Reminder\","
            "\"start\":\"2026-09-03T10:00:00\",\"duration\":\"PT1H\","
            "\"showWithoutTime\":false}','event-state')")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO calendar_occurrences(account_id,occurrence_id,event_id,recurrence_id,"
            "start_utc,end_utc,local_start,local_end,is_all_day) VALUES "
            "('owner','event-1','event-1',NULL,'2026-09-02T22:00:00Z',"
            "'2026-09-02T23:00:00Z','2026-09-03T10:00:00','2026-09-03T11:00:00',0)")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO calendar_reminder_horizons(account_id,range_start,range_end,"
            "display_time_zone) VALUES('owner','2026-09-02T00:00:00','2026-12-03T00:00:00',"
            "'Pacific/Auckland')")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO calendar_reminder_occurrences(account_id,occurrence_id) "
                           "VALUES('owner','event-1')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery migratedTiming{migrated.database()};
    REQUIRE(migratedTiming.exec(QStringLiteral(
        "SELECT start_utc,end_utc FROM calendar_reminder_occurrences WHERE account_id='owner' AND "
        "occurrence_id='event-1'")));
    REQUIRE(migratedTiming.next());
    CHECK(migratedTiming.value(0).isNull());
    CHECK(migratedTiming.value(1).isNull());

    QSqlQuery removeOwner{migrated.database()};
    REQUIRE(removeOwner.exec(QStringLiteral("DELETE FROM accounts WHERE account_id='owner'")));
    QSqlQuery retained{migrated.database()};
    REQUIRE(retained.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM calendar_reminder_horizons),"
                                         "(SELECT COUNT(*) FROM calendar_reminder_occurrences)")));
    REQUIRE(retained.next());
    CHECK(retained.value(0).toInt() == 0);
    CHECK(retained.value(1).toInt() == 0);
}

TEST_CASE("legacy mail notification state is discarded without touching cached mail",
          "[jmap][cache][database][notification]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("legacy-mail-notification-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto notificationState = std::ranges::find_if(
            currentRunner.steps(), [](const auto& step) { return step.version == 65; });
        REQUIRE(notificationState != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     notificationState};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id,subject,preview) "
                           "VALUES('account-1','email-1','thread-1','Subject','Preview')")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO observed_notification_emails(account_id,email_id) "
                           "VALUES('account-1','email-1')")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO mail_notification_outbox(account_id,mailbox_id,email_id,thread_id,subject,"
            "received_at,status) VALUES('account-1','inbox','email-1','thread-1','Subject',"
            "'2026-08-27T00:00:00Z','pending')")));
        REQUIRE(seed.exec(QStringLiteral("INSERT INTO notification_dispatch_claims(kind,claim_key) "
                                         "VALUES('mail','legacy-claim')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery email{migrated.database()};
    REQUIRE(email.exec(QStringLiteral(
        "SELECT subject FROM emails WHERE account_id='account-1' AND email_id='email-1'")));
    REQUIRE(email.next());
    CHECK(email.value(0).toString() == QStringLiteral("Subject"));

    QSqlQuery legacyTables{migrated.database()};
    REQUIRE(legacyTables.exec(
        QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN "
                       "('observed_notification_emails','mail_notification_outbox')")));
    REQUIRE(legacyTables.next());
    CHECK(legacyTables.value(0).toInt() == 0);

    QSqlQuery newState{migrated.database()};
    REQUIRE(newState.exec(
        QStringLiteral("SELECT (SELECT COUNT(*) FROM mail_notification_state),"
                       "(SELECT COUNT(*) FROM mail_notification_event_outbox),"
                       "(SELECT COUNT(*) FROM notification_dispatch_claims WHERE kind='mail')")));
    REQUIRE(newState.next());
    CHECK(newState.value(0).toInt() == 0);
    CHECK(newState.value(1).toInt() == 0);
    CHECK(newState.value(2).toInt() == 0);
}

TEST_CASE(
    "stale notification horizon migration preserves the armed mailbox without its Email token",
    "[jmap][cache][database][notification][migration]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("stale-notification-horizon-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto simplifiedMailboxes = std::ranges::find_if(
            currentRunner.steps(), [](const auto& step) { return step.version == 68; });
        REQUIRE(simplifiedMailboxes != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     simplifiedMailboxes};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO sync_state(account_id,object_type,query_key,state_token) "
                           "VALUES('account-1','Email','','email-state-current')")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO mail_notification_horizons(account_id,mailbox_id,email_state,enabled_at) "
            "VALUES('account-1','inbox','email-state-stale','2026-08-27 18:51:14')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery active{migrated.database()};
    REQUIRE(active.exec(QStringLiteral(
        "SELECT mailbox_id FROM mail_notification_mailboxes WHERE account_id='account-1'")));
    REQUIRE(active.next());
    CHECK(active.value(0).toString() == QStringLiteral("inbox"));
    CHECK_FALSE(active.next());

    QSqlQuery retired{migrated.database()};
    REQUIRE(retired.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                                        "name='mail_notification_horizons'")));
    REQUIRE(retired.next());
    CHECK(retired.value(0).toInt() == 0);

    QSqlQuery columns{migrated.database()};
    REQUIRE(columns.exec(QStringLiteral("PRAGMA table_info(mail_notification_mailboxes)")));
    std::vector<QString> columnNames;
    while (columns.next())
        columnNames.push_back(columns.value(1).toString());
    CHECK(columnNames ==
          std::vector<QString>{QStringLiteral("account_id"), QStringLiteral("mailbox_id")});
}

TEST_CASE("mutation journal retention migrates and follows durable owners",
          "[jmap][cache][database][mutation-journal]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("legacy-mutation-retention-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        const auto retention = std::ranges::find_if(currentRunner.steps(), [](const auto& step)
                                                    { return step.version == 63; });
        REQUIRE(retention != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     retention};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        REQUIRE_FALSE(legacyRunner.migrate(fixture).has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO operation_history(entry_id,stack,stack_order,domain,command_kind,label,"
            "payload_version,payload_json,status,operation_group_id) VALUES "
            "('history-1','undo',1,'mail','test','Test',1,'{}','preparing','group-history'),"
            "('history-2','undo',2,'mail','test','Executing',1,'{}','executing_undo',"
            "'group-executing')")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO background_jobs(job_id,kind,priority,status,title) VALUES "
                           "('group-job','test',1,'queued','Test job')")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO mail_transfer_operations(operation_id,operation_group_id,"
            "source_account_id,destination_account_id,destination_mailbox_id,operation,topology,"
            "status) VALUES('transfer-1','group-transfer','account-1','account-1','mailbox-1',"
            "'copy','same_session_copy','running')")));
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO mutation_journal(mutation_id,operation_group_id,account_id,data_type,"
            "object_id,mutation_kind,status,payload_json,sequence) VALUES "
            "('orphan-terminal',NULL,'account-1','ContactCard','card-1','test','rejected','{}',1),"
            "('history-terminal','group-history','account-1','ContactCard','card-2','test',"
            "'accepted','{}',2),"
            "('job-terminal','group-job','account-1','Email','email-1','test','rejected','{}',3),"
            "('transfer-terminal','group-transfer','account-1','Email','email-2','test','accepted',"
            "'{}',4),"
            "('unresolved',NULL,'account-1','CalendarEvent','event-1','test','unknown','{}',5),"
            "('chain-terminal',NULL,'account-1','Email','email-chain','test','accepted','{}',6),"
            "('chain-active',NULL,'account-1','Email','email-chain','test','pending','{}',7),"
            "('executing-terminal','group-executing','account-1','Email','email-3','test',"
            "'accepted','{}',8)")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));
    CHECK(migrated.schemaVersion() == 73);

    QSqlQuery retained{migrated.database()};
    REQUIRE(retained.exec(
        QStringLiteral("SELECT mutation_id FROM mutation_journal ORDER BY sequence")));
    std::vector<QString> mutationIds;
    while (retained.next())
        mutationIds.push_back(retained.value(0).toString());
    CHECK(mutationIds ==
          std::vector<QString>{QStringLiteral("history-terminal"), QStringLiteral("job-terminal"),
                               QStringLiteral("transfer-terminal"), QStringLiteral("unresolved"),
                               QStringLiteral("chain-terminal"), QStringLiteral("chain-active"),
                               QStringLiteral("executing-terminal")});

    QSqlQuery settle{migrated.database()};
    REQUIRE(settle.exec(
        QStringLiteral("UPDATE operation_history SET status='ready' WHERE entry_id='history-1'")));
    REQUIRE(settle.exec(
        QStringLiteral("UPDATE operation_history SET status='ready' WHERE entry_id='history-2'")));
    REQUIRE(settle.exec(
        QStringLiteral("UPDATE background_jobs SET status='complete' WHERE job_id='group-job'")));
    REQUIRE(settle.exec(QStringLiteral(
        "UPDATE mail_transfer_operations SET status='complete' WHERE operation_id='transfer-1'")));
    REQUIRE(settle.exec(QStringLiteral(
        "UPDATE mutation_journal SET status='rejected' WHERE mutation_id='unresolved'")));
    REQUIRE(settle.exec(QStringLiteral(
        "UPDATE mutation_journal SET status='accepted' WHERE mutation_id='chain-active'")));
    REQUIRE(settle.exec(
        QStringLiteral("DELETE FROM mutation_journal WHERE mutation_id IN (SELECT mutation_id FROM "
                       "mutation_journal_retireable_terminal)")));
    REQUIRE(settle.exec(QStringLiteral("SELECT COUNT(*) FROM mutation_journal")));
    REQUIRE(settle.next());
    CHECK(settle.value(0).toInt() == 0);
}

TEST_CASE("thread membership migration normalizes JSON members in order",
          "[jmap][cache][database][thread-coverage]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath =
        temporaryDir.filePath(QStringLiteral("legacy-thread-cache.sqlite3"));
    const QString fixtureConnectionName = makeConnectionName();
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), fixtureConnectionName);
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());

        const auto currentRunner = javelin::jmap::cache::createDefaultMigrationRunner();
        REQUIRE(currentRunner.steps().size() >= 2);
        const auto normalization = std::ranges::find_if(currentRunner.steps(), [](const auto& step)
                                                        { return step.version == 47; });
        REQUIRE(normalization != currentRunner.steps().end());
        std::vector<javelin::jmap::cache::MigrationStep> legacySteps{currentRunner.steps().begin(),
                                                                     normalization};
        const javelin::jmap::cache::MigrationRunner legacyRunner{std::move(legacySteps)};
        const auto migrationError = legacyRunner.migrate(fixture);
        REQUIRE_FALSE(migrationError.has_value());

        QSqlQuery seed{fixture};
        REQUIRE(seed.exec(QStringLiteral(
            "INSERT INTO accounts(account_id,email_address,session_url,is_primary) "
            "VALUES('account-1','alice@example.test','https://example.test/jmap',1)")));
        REQUIRE(seed.exec(
            QStringLiteral("INSERT INTO threads(account_id,thread_id,email_ids_json,state) "
                           "VALUES('account-1','thread-1','[\"email-2\",\"email-1\",\"email-3\"]',"
                           "'thread-state-1')")));
        seed.finish();
        fixture.close();
    }
    QSqlDatabase::removeDatabase(fixtureConnectionName);

    auto migratedResult = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&migratedResult))
        FAIL(error->message.toStdString());
    auto migrated = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(migratedResult));

    QSqlQuery columns{migrated.database()};
    REQUIRE(columns.exec(QStringLiteral("PRAGMA table_info(threads)")));
    bool hasLegacyJson = false;
    bool hasFreshness = false;
    bool hasMemberCount = false;
    while (columns.next())
    {
        const auto name = columns.value(1).toString();
        hasLegacyJson = hasLegacyJson || name == QStringLiteral("email_ids_json");
        hasFreshness = hasFreshness || name == QStringLiteral("membership_freshness");
        hasMemberCount = hasMemberCount || name == QStringLiteral("member_count");
    }
    CHECK_FALSE(hasLegacyJson);
    CHECK(hasFreshness);
    CHECK(hasMemberCount);

    QSqlQuery thread{migrated.database()};
    REQUIRE(
        thread.exec(QStringLiteral("SELECT membership_freshness,member_count,state FROM threads "
                                   "WHERE account_id='account-1' AND thread_id='thread-1'")));
    REQUIRE(thread.next());
    CHECK(thread.value(0).toString() == QStringLiteral("current"));
    CHECK(thread.value(1).toInt() == 3);
    CHECK(thread.value(2).toString() == QStringLiteral("thread-state-1"));

    QSqlQuery members{migrated.database()};
    REQUIRE(members.exec(QStringLiteral(
        "SELECT position,email_id FROM thread_email_members WHERE account_id='account-1' AND "
        "thread_id='thread-1' ORDER BY position")));
    std::vector<std::pair<int, QString>> orderedMembers;
    while (members.next())
        orderedMembers.emplace_back(members.value(0).toInt(), members.value(1).toString());
    CHECK(orderedMembers == std::vector<std::pair<int, QString>>{{0, QStringLiteral("email-2")},
                                                                 {1, QStringLiteral("email-1")},
                                                                 {2, QStringLiteral("email-3")}});
}

TEST_CASE("GUI database factory opens an existing cache read-only", "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));

    auto daemonResult = javelin::jmap::cache::DaemonDatabaseFactory{
        javelin::jmap::cache::DatabaseConnectionOptions{
            .connectionName = makeConnectionName(),
            .databasePath = databasePath,
        }}.open();
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(daemonResult));
    auto daemon = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(daemonResult));
    const auto expectedSchema =
        javelin::jmap::cache::createDefaultMigrationRunner().latestVersion();
    const auto writerDataVersion = daemon.dataVersion();
    REQUIRE(std::holds_alternative<std::uint64_t>(writerDataVersion));
    daemon = {};

    auto guiResult = javelin::jmap::cache::GuiDatabaseFactory{
        javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
            .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
            .databasePath = databasePath,
            .busyTimeout = std::chrono::milliseconds{125},
        }}.openForCurrentThread("mail-list");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(guiResult));
    auto gui = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(guiResult));

    CHECK(gui.schemaVersion() == expectedSchema);
    const auto guiDataVersion = gui.dataVersion();
    REQUIRE(std::holds_alternative<std::uint64_t>(guiDataVersion));
    CHECK(std::get<std::uint64_t>(guiDataVersion) == std::get<std::uint64_t>(writerDataVersion));
    CHECK(pragmaValue(gui.database(), QStringLiteral("query_only")) == QStringLiteral("1"));
    CHECK(pragmaValue(gui.database(), QStringLiteral("busy_timeout")) == QStringLiteral("125"));

    QSqlQuery writeQuery{gui.database()};
    CHECK_FALSE(writeQuery.exec(QStringLiteral("CREATE TABLE must_not_be_created(value TEXT)")));
    QSqlQuery schemaQuery{gui.database()};
    REQUIRE(schemaQuery.exec(
        QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='must_not_be_created'")));
    REQUIRE(schemaQuery.next());
    CHECK(schemaQuery.value(0).toInt() == 0);
}

TEST_CASE("GUI database factory never migrates a legacy SQLite file", "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath = temporaryDir.filePath(QStringLiteral("legacy.sqlite3"));
    {
        QSqlDatabase fixture =
            QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), makeConnectionName());
        fixture.setDatabaseName(databasePath);
        REQUIRE(fixture.open());
        QSqlQuery create{fixture};
        REQUIRE(create.exec(QStringLiteral("CREATE TABLE legacy_probe(value TEXT)")));
        fixture.close();
        const QString fixtureName = fixture.connectionName();
        fixture = QSqlDatabase{};
        QSqlDatabase::removeDatabase(fixtureName);
    }

    auto guiResult = javelin::jmap::cache::GuiDatabaseFactory{
        javelin::jmap::cache::ReadOnlyThreadConnectionFactoryOptions{
            .connectionNamePrefix = QStringLiteral("javelin-gui-read"),
            .databasePath = databasePath,
        }}.openForCurrentThread("legacy");
    REQUIRE(std::holds_alternative<javelin::jmap::cache::ReadOnlyDatabaseConnection>(guiResult));
    auto gui = std::get<javelin::jmap::cache::ReadOnlyDatabaseConnection>(std::move(guiResult));

    CHECK(gui.schemaVersion() == 0);
    QSqlQuery probe{gui.database()};
    REQUIRE(probe.exec(
        QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='schema_migrations'")));
    REQUIRE(probe.next());
    CHECK(probe.value(0).toInt() == 0);
}

TEST_CASE("thread connection factory releases its Qt connection when the wrapper is destroyed",
          "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    const javelin::jmap::cache::ThreadConnectionFactory factory{
        javelin::jmap::cache::ThreadConnectionFactoryOptions{
            .connectionNamePrefix = QStringLiteral("javelin-cache"),
            .databasePath = databasePath,
        }};

    auto firstOpen = factory.openForCurrentThread("gui");
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&firstOpen))
    {
        FAIL(error->message.toStdString());
    }

    auto firstConnection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(firstOpen));
    const QString connectionName = firstConnection.connectionName();
    CHECK(QSqlDatabase::contains(connectionName));

    firstConnection = {};
    CHECK_FALSE(QSqlDatabase::contains(connectionName));

    auto secondOpen = factory.openForCurrentThread("gui");
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&secondOpen))
    {
        FAIL(error->message.toStdString());
    }

    auto secondConnection =
        std::get<javelin::jmap::cache::DatabaseConnection>(std::move(secondOpen));
    CHECK(QSqlDatabase::contains(secondConnection.connectionName()));
}

TEST_CASE("database write coordination serializes real connections with no SQLite busy timeout",
          "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    auto initialOpen = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(initialOpen));
    auto initial = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(initialOpen));
    QSqlQuery createProbe{initial.database()};
    REQUIRE(createProbe.exec(QStringLiteral(
        "CREATE TABLE write_coordination_probe(key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT")));

    std::atomic_int ready = 0;
    std::atomic_bool start = false;
    std::atomic_bool firstHasTransaction = false;
    std::atomic_bool firstSucceeded = false;
    std::atomic_bool secondSucceeded = false;
    const auto openWorkerConnection = [&](const QString& name)
    {
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = name,
            .databasePath = databasePath,
            .busyTimeout = std::chrono::milliseconds{0},
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
        {
            Q_UNUSED(error);
            return std::optional<javelin::jmap::cache::DatabaseConnection>{};
        }
        return std::optional<javelin::jmap::cache::DatabaseConnection>{
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened))};
    };
    std::thread first{
        [&]()
        {
            auto connection = openWorkerConnection(QStringLiteral("coordinated-writer-first"));
            ready.fetch_add(1, std::memory_order_release);
            if (!connection)
            {
                firstHasTransaction.store(true, std::memory_order_release);
                return;
            }
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                *connection, QStringLiteral("First coordinated write"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            {
                Q_UNUSED(error);
                firstHasTransaction.store(true, std::memory_order_release);
                return;
            }
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
            firstHasTransaction.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
            QSqlQuery query{connection->database()};
            const bool inserted = query.exec(QStringLiteral(
                "INSERT INTO write_coordination_probe(key,value) VALUES('first','one')"));
            firstSucceeded.store(inserted && !transaction.commit().has_value(),
                                 std::memory_order_release);
        }};
    std::thread second{
        [&]()
        {
            auto connection = openWorkerConnection(QStringLiteral("coordinated-writer-second"));
            ready.fetch_add(1, std::memory_order_release);
            if (!connection)
                return;
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            while (!firstHasTransaction.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                *connection, QStringLiteral("Second coordinated write"));
            if (const auto* error =
                    std::get_if<javelin::jmap::cache::DatabaseError>(&transactionResult))
            {
                Q_UNUSED(error);
                return;
            }
            auto transaction =
                std::get<javelin::jmap::cache::DatabaseTransaction>(std::move(transactionResult));
            QSqlQuery query{connection->database()};
            const bool inserted = query.exec(QStringLiteral(
                "INSERT INTO write_coordination_probe(key,value) VALUES('second','two')"));
            secondSucceeded.store(inserted && !transaction.commit().has_value(),
                                  std::memory_order_release);
        }};
    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    CHECK(firstSucceeded.load(std::memory_order_acquire));
    CHECK(secondSucceeded.load(std::memory_order_acquire));
    QSqlQuery count{initial.database()};
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM write_coordination_probe")));
    REQUIRE(count.next());
    CHECK(count.value(0).toInt() == 2);
}

TEST_CASE("external SQLite writer contention is classified as transient", "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
        .busyTimeout = std::chrono::milliseconds{0},
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    const QString externalName = QStringLiteral("uncoordinated-external-writer");
    {
        auto external = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), externalName);
        external.setDatabaseName(databasePath);
        REQUIRE(external.open());
        {
            QSqlQuery lock{external};
            REQUIRE(lock.exec(QStringLiteral("BEGIN IMMEDIATE TRANSACTION")));
        }

        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
            connection, QStringLiteral("Contended coordinated write"));
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseError>(transactionResult));
        CHECK(std::get<javelin::jmap::cache::DatabaseError>(transactionResult).code ==
              javelin::jmap::cache::DatabaseErrorCode::TransientContention);
        javelin::jmap::cache::SyncStateRepository states{connection};
        const auto upsertError =
            states.upsert({.accountId = "account", .objectType = "Email", .queryKey = {}}, "state");
        REQUIRE(upsertError.has_value());
        CHECK(upsertError->code == javelin::jmap::cache::DatabaseErrorCode::TransientContention);
        external.rollback();
        external.close();
    }
    QSqlDatabase::removeDatabase(externalName);
}

TEST_CASE("database connections reject cross-thread access", "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
    std::optional<javelin::jmap::cache::DatabaseError> crossThreadError;
    std::thread worker{[&]() { crossThreadError = connection.validate(); }};
    worker.join();

    REQUIRE(crossThreadError.has_value());
    CHECK(crossThreadError->code ==
          javelin::jmap::cache::DatabaseErrorCode::ThreadAffinityViolation);
}

TEST_CASE("transactions and autocommit writes remain safe under mixed connection load",
          "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());
    const QString databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3"));
    auto initialOpen = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = databasePath,
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(initialOpen));
    auto initial = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(initialOpen));
    QSqlQuery createProbe{initial.database()};
    REQUIRE(createProbe.exec(QStringLiteral(
        "CREATE TABLE mixed_write_probe(key TEXT PRIMARY KEY,value TEXT NOT NULL) STRICT")));

    constexpr int writerCount = 4;
    constexpr int writesPerWriter = 25;
    std::atomic_bool start = false;
    std::atomic_int failures = 0;
    std::vector<std::thread> writers;
    writers.reserve(writerCount);
    for (int writer = 0; writer < writerCount; ++writer)
    {
        writers.emplace_back(
            [&, writer]()
            {
                auto opened = javelin::jmap::cache::DatabaseConnection::open({
                    .connectionName = QStringLiteral("mixed-writer-%1").arg(writer),
                    .databasePath = databasePath,
                    .busyTimeout = std::chrono::milliseconds{0},
                });
                if (!std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened))
                {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                auto connection =
                    std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int item = 0; item < writesPerWriter; ++item)
                {
                    std::optional<javelin::jmap::cache::DatabaseTransaction> transaction;
                    std::optional<javelin::jmap::cache::DatabaseWriteScope> autocommitScope;
                    if (writer % 2 == 0)
                    {
                        auto transactionResult = javelin::jmap::cache::DatabaseTransaction::begin(
                            connection, QStringLiteral("Mixed load transaction"));
                        if (!std::holds_alternative<javelin::jmap::cache::DatabaseTransaction>(
                                transactionResult))
                        {
                            failures.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        transaction.emplace(std::get<javelin::jmap::cache::DatabaseTransaction>(
                            std::move(transactionResult)));
                    }
                    else
                    {
                        autocommitScope.emplace(connection);
                    }
                    QSqlQuery insert{connection.database()};
                    insert.prepare(QStringLiteral(
                        "INSERT INTO mixed_write_probe(key,value) VALUES(:key,:value)"));
                    const QString key = QStringLiteral("%1-%2").arg(writer).arg(item);
                    insert.bindValue(QStringLiteral(":key"), key);
                    insert.bindValue(QStringLiteral(":value"), key);
                    if (!insert.exec())
                        failures.fetch_add(1, std::memory_order_relaxed);
                    if (transaction && transaction->commit())
                        failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
    }
    start.store(true, std::memory_order_release);
    for (auto& writer : writers)
        writer.join();

    CHECK(failures.load(std::memory_order_acquire) == 0);
    QSqlQuery count{initial.database()};
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM mixed_write_probe")));
    REQUIRE(count.next());
    CHECK(count.value(0).toInt() == writerCount * writesPerWriter);
}
