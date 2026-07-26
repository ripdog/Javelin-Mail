#include "jmap/cache/Database.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QCoreApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QStringList>

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
    REQUIRE(migrations.size() == 30);
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
    CHECK(migrations.back().version == 32);
    CHECK(migrations.back().name == QStringLiteral("operation_history"));

    QSqlQuery tableQuery{connection.database()};
    REQUIRE(tableQuery.exec(
        QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
                       "('accounts', 'compose_sessions', 'mailboxes', "
                       "'emails', 'jmap_transport_preferences', 'observed_notification_emails', "
                       "'mail_notification_outbox', "
                       "'raw_message_sources', 'mail_vault_objects', 'mail_vault_email_refs', "
                       "'offline_mailbox_scopes', 'background_jobs', "
                       "'schema_migrations', 'translation_cache', 'search_windows', "
                       "'search_window_items', 'mailbox_query_windows', "
                       "'mailbox_query_window_items', 'sync_state', 'consistency_domains', "
                       "'mutation_journal', 'calendar_notification_state', "
                       "'calendar_default_alerts', 'operation_history', "
                       "'operation_history_sequence') "
                       "ORDER BY name")));

    QStringList tableNames;
    while (tableQuery.next())
    {
        tableNames.push_back(tableQuery.value(0).toString());
    }

    CHECK(tableNames == QStringList{QStringLiteral("accounts"),
                                    QStringLiteral("background_jobs"),
                                    QStringLiteral("calendar_default_alerts"),
                                    QStringLiteral("calendar_notification_state"),
                                    QStringLiteral("compose_sessions"),
                                    QStringLiteral("consistency_domains"),
                                    QStringLiteral("emails"),
                                    QStringLiteral("jmap_transport_preferences"),
                                    QStringLiteral("mail_notification_outbox"),
                                    QStringLiteral("mail_vault_email_refs"),
                                    QStringLiteral("mail_vault_objects"),
                                    QStringLiteral("mailbox_query_window_items"),
                                    QStringLiteral("mailbox_query_windows"),
                                    QStringLiteral("mailboxes"),
                                    QStringLiteral("mutation_journal"),
                                    QStringLiteral("observed_notification_emails"),
                                    QStringLiteral("offline_mailbox_scopes"),
                                    QStringLiteral("operation_history"),
                                    QStringLiteral("operation_history_sequence"),
                                    QStringLiteral("raw_message_sources"),
                                    QStringLiteral("schema_migrations"),
                                    QStringLiteral("search_window_items"),
                                    QStringLiteral("search_windows"),
                                    QStringLiteral("sync_state"),
                                    QStringLiteral("translation_cache")});
    CHECK(pragmaValue(connection.database(), QStringLiteral("foreign_keys")) ==
          QStringLiteral("1"));
    CHECK(pragmaValue(connection.database(), QStringLiteral("journal_mode"))
              .compare(QStringLiteral("wal"), Qt::CaseInsensitive) == 0);
    CHECK(pragmaValue(connection.database(), QStringLiteral("busy_timeout")) ==
          QStringLiteral("5000"));
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
    REQUIRE(migrations.size() == 30);
    CHECK(migrations.front().version == 1);
    CHECK(migrations.at(1).version == 2);
    CHECK(migrations.at(2).version == 3);
    CHECK(migrations.at(6).version == 9);
    CHECK(migrations.back().version == 32);
    CHECK(connection.schemaVersion() == 32);
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
    CHECK(pragmaValue(firstConnection.database(), QStringLiteral("busy_timeout")) ==
          QStringLiteral("30000"));
    const QString expectedName =
        QStringLiteral("javelin-cache-gui-thread-%1")
            .arg(javelin::jmap::cache::ThreadConnectionFactory::currentThreadTag());
    CHECK(firstConnection.connectionName() == expectedName);
    CHECK(QSqlDatabase::contains(expectedName));

    firstConnection = {};
    CHECK_FALSE(QSqlDatabase::contains(expectedName));

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
                "INSERT INTO "
                "translation_cache(source_language,target_language,input_hash,input_text,"
                "translated_text) VALUES('en','fr','first','one','un')"));
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
                "INSERT INTO "
                "translation_cache(source_language,target_language,input_hash,input_text,"
                "translated_text) VALUES('en','fr','second','two','deux')"));
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
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM translation_cache")));
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
                        "INSERT INTO translation_cache(source_language,target_language,input_hash,"
                        "input_text,translated_text) VALUES('en','fr',:hash,:input,:translation)"));
                    const QString key = QStringLiteral("%1-%2").arg(writer).arg(item);
                    insert.bindValue(QStringLiteral(":hash"), key);
                    insert.bindValue(QStringLiteral(":input"), key);
                    insert.bindValue(QStringLiteral(":translation"), key);
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
    REQUIRE(count.exec(QStringLiteral("SELECT COUNT(*) FROM translation_cache")));
    REQUIRE(count.next());
    CHECK(count.value(0).toInt() == writerCount * writesPerWriter);
}
