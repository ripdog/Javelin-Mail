#include "jmap/sync/SyncPlanner.h"

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <array>
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
        return QStringLiteral("javelin-sync-plan-%1").arg(counter);
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

} // namespace

TEST_CASE("sync planner requests an initial fetch when no cached state exists", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    const javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    const javelin::jmap::sync::SyncPlanner planner{syncStateRepository};

    const auto result = planner.plan({
        .accountId = "account-1",
        .objectType = "Mailbox",
        .queryKey = "",
    });

    REQUIRE(std::holds_alternative<javelin::jmap::sync::SyncPlan>(result));
    const auto& plan = std::get<javelin::jmap::sync::SyncPlan>(result);
    CHECK(plan.kind == javelin::jmap::sync::SyncPlanKind::InitialFetch);
    CHECK_FALSE(plan.sinceState.has_value());
}

TEST_CASE("sync planner requests incremental changes when a cached state token exists",
          "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert(
                          {
                              .accountId = "account-1",
                              .objectType = "Email",
                              .queryKey = "mailbox:mbx-inbox",
                          },
                          "state-42")
                      .has_value());

    const javelin::jmap::sync::SyncPlanner planner{syncStateRepository};
    const auto result = planner.plan({
        .accountId = "account-1",
        .objectType = "Email",
        .queryKey = "mailbox:mbx-inbox",
    });

    REQUIRE(std::holds_alternative<javelin::jmap::sync::SyncPlan>(result));
    const auto& plan = std::get<javelin::jmap::sync::SyncPlan>(result);
    CHECK(plan.kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges);
    REQUIRE(plan.sinceState.has_value());
    CHECK(*plan.sinceState == "state-42");
}

TEST_CASE("sync planner batches mixed initial and incremental plans in call order", "[jmap][sync]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert(
                          {
                              .accountId = "account-1",
                              .objectType = "Mailbox",
                              .queryKey = "",
                          },
                          "mailbox-state")
                      .has_value());

    const javelin::jmap::sync::SyncPlanner planner{syncStateRepository};
    const std::array keys{
        javelin::jmap::cache::SyncStateKey{
            .accountId = "account-1",
            .objectType = "Mailbox",
            .queryKey = "",
        },
        javelin::jmap::cache::SyncStateKey{
            .accountId = "account-1",
            .objectType = "Email",
            .queryKey = "mailbox:mbx-inbox",
        },
    };

    const auto result = planner.planMany(keys);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::SyncPlan>>(result));
    const auto& plans = std::get<std::vector<javelin::jmap::sync::SyncPlan>>(result);
    REQUIRE(plans.size() == 2);
    CHECK(plans.front().kind == javelin::jmap::sync::SyncPlanKind::IncrementalChanges);
    CHECK(plans.back().kind == javelin::jmap::sync::SyncPlanKind::InitialFetch);
    REQUIRE(plans.front().sinceState.has_value());
    CHECK(*plans.front().sinceState == "mailbox-state");
    CHECK(plans.back().key.queryKey == "mailbox:mbx-inbox");
}
