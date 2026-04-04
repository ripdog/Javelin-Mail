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

    const QString databasePath = temporaryDir.filePath("cache.sqlite3");
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
    REQUIRE(migrations.size() == 1);
    CHECK(migrations.front().version == 1);
    CHECK(migrations.front().name == "initial_cache_schema");

    QSqlQuery tableQuery{connection.database()};
    REQUIRE(tableQuery.exec(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
        "('accounts', 'mailboxes', 'emails', 'schema_migrations', 'sync_state') ORDER BY name"));

    QStringList tableNames;
    while (tableQuery.next())
    {
        tableNames.push_back(tableQuery.value(0).toString());
    }

    CHECK(tableNames ==
          QStringList{"accounts", "emails", "mailboxes", "schema_migrations", "sync_state"});
    CHECK(pragmaValue(connection.database(), "foreign_keys") == "1");
    CHECK(pragmaValue(connection.database(), "journal_mode").compare("wal", Qt::CaseInsensitive) ==
          0);
}

TEST_CASE("database migrations are repeatable when reopening an existing cache",
          "[jmap][cache][database]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    const QString databasePath = temporaryDir.filePath("cache.sqlite3");
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
    REQUIRE(migrations.size() == 1);
    CHECK(migrations.front().version == 1);
    CHECK(connection.schemaVersion() == 1);
}
