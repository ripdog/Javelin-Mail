#include "jmap/cache/Database.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

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
        return QStringLiteral("javelin-test-db-%1").arg(counter);
    }

    [[nodiscard]] QString pragmaValue(QSqlDatabase& database, const QString& name)
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
    REQUIRE(migrations.size() == 24);
    CHECK(migrations.front().version == 1);
    CHECK(migrations.front().name == QStringLiteral("initial_cache_schema"));
    CHECK(migrations.at(1).version == 2);
    CHECK(migrations.at(1).name == QStringLiteral("mailboxes_is_subscribed"));
    CHECK(migrations.at(2).version == 3);
    CHECK(migrations.at(2).name == QStringLiteral("session_and_account_metadata"));
    CHECK(migrations.at(3).version == 4);
    CHECK(migrations.at(3).name == QStringLiteral("compose_and_threading_metadata"));
    CHECK(migrations.at(4).version == 5);
    CHECK(migrations.at(4).name == QStringLiteral("raw_message_sources"));
    CHECK(migrations.at(5).version == 8);
    CHECK(migrations.at(5).name == QStringLiteral("account_session_ownership"));
    CHECK(migrations.at(6).version == 9);
    CHECK(migrations.at(6).name == QStringLiteral("ensure_raw_message_sources"));
    CHECK(migrations.at(7).version == 10);
    CHECK(migrations.at(7).name == QStringLiteral("translation_cache"));
    CHECK(migrations.at(8).version == 11);
    CHECK(migrations.at(8).name == QStringLiteral("contacts_capabilities"));
    CHECK(migrations.at(10).version == 13);
    CHECK(migrations.at(10).name == QStringLiteral("websocket_push_capability"));
    CHECK(migrations.at(11).version == 14);
    CHECK(migrations.at(11).name == QStringLiteral("search_windows"));
    CHECK(migrations.back().version == 26);
    CHECK(migrations.back().name == QStringLiteral("calendar_default_alerts"));

    QSqlQuery tableQuery{connection.database()};
    REQUIRE(tableQuery.exec(
        QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
                       "('accounts', 'compose_sessions', 'mailboxes', "
                       "'emails', 'jmap_transport_preferences', 'observed_notification_emails', "
                       "'raw_message_sources', 'email_search_fts', "
                       "'schema_migrations', 'translation_cache', 'search_windows', "
                       "'search_window_items', 'mailbox_query_windows', "
                       "'mailbox_query_window_items', 'sync_state', 'consistency_domains', "
                       "'mutation_journal', 'calendar_notification_state', "
                       "'calendar_default_alerts') "
                       "ORDER BY name")));

    QStringList tableNames;
    while (tableQuery.next())
    {
        tableNames.push_back(tableQuery.value(0).toString());
    }

    CHECK(tableNames ==
          QStringList{
              QStringLiteral("accounts"), QStringLiteral("calendar_default_alerts"),
              QStringLiteral("calendar_notification_state"), QStringLiteral("compose_sessions"),
              QStringLiteral("consistency_domains"), QStringLiteral("email_search_fts"),
              QStringLiteral("emails"), QStringLiteral("jmap_transport_preferences"),
              QStringLiteral("mailbox_query_window_items"), QStringLiteral("mailbox_query_windows"),
              QStringLiteral("mailboxes"), QStringLiteral("mutation_journal"),
              QStringLiteral("observed_notification_emails"), QStringLiteral("raw_message_sources"),
              QStringLiteral("schema_migrations"), QStringLiteral("search_window_items"),
              QStringLiteral("search_windows"), QStringLiteral("sync_state"),
              QStringLiteral("translation_cache")});
    CHECK(pragmaValue(connection.database(), QStringLiteral("foreign_keys")) ==
          QStringLiteral("1"));
    CHECK(pragmaValue(connection.database(), QStringLiteral("journal_mode"))
              .compare(QStringLiteral("wal"), Qt::CaseInsensitive) == 0);
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
    REQUIRE(migrations.size() == 24);
    CHECK(migrations.front().version == 1);
    CHECK(migrations.at(1).version == 2);
    CHECK(migrations.at(2).version == 3);
    CHECK(migrations.at(6).version == 9);
    CHECK(migrations.back().version == 26);
    CHECK(connection.schemaVersion() == 26);
}

TEST_CASE("thread connection factory encodes owner tag and current thread in connection names",
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
    const QString expectedName =
        QStringLiteral("javelin-cache-gui-thread-%1")
            .arg(javelin::jmap::cache::ThreadConnectionFactory::currentThreadTag());
    CHECK(firstConnection.connectionName() == expectedName);

    firstConnection = {};

    auto secondOpen = factory.openForCurrentThread("gui");
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&secondOpen))
    {
        FAIL(error->message.toStdString());
    }

    auto secondConnection =
        std::get<javelin::jmap::cache::DatabaseConnection>(std::move(secondOpen));
    CHECK(secondConnection.connectionName() == expectedName);
    CHECK(secondConnection.schemaVersion() ==
          javelin::jmap::cache::createDefaultMigrationRunner().latestVersion());
}
