#include "jmap/sync/MailboxStateRefreshExecutor.h"
#include "FixtureReader.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/MailboxRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/domain/MailEntityParsers.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <variant>
#include <vector>

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

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            REQUIRE_FALSE(queuedResults.empty());

            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            co_return result;
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-mailbox-state-refresh-%1").arg(counter);
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

    [[nodiscard]] javelin::jmap::api::ApiRequestContext makeRequestContext()
    {
        return javelin::jmap::api::ApiRequestContext{
            .credentials =
                {
                    .accountId = "account-1",
                    .emailAddress = "alice@example.com",
                    .sessionUrl = "https://mail.example.com/.well-known/jmap",
                    .token =
                        {
                            .accessToken = "access-token",
                            .refreshToken = std::nullopt,
                            .expiry = std::nullopt,
                        },
                },
            .apiUrl = "https://mail.example.com/jmap/api",
        };
    }

    [[nodiscard]] std::string
    serializeResponseEnvelope(const javelin::jmap::api::ResponseEnvelope& envelope)
    {
        const auto serialized = javelin::jmap::api::serializeResponseEnvelope(envelope);
        REQUIRE(serialized.has_value());
        return *serialized;
    }

    [[nodiscard]] std::string mailboxGetArguments(std::string state, std::string mailboxJson)
    {
        return std::string{R"({"accountId":"account-1","state":")"} + std::move(state) +
               R"(","list":[)" + std::move(mailboxJson) + R"(],"notFound":[]})";
    }

    [[nodiscard]] std::string updatedMailboxFixture()
    {
        auto updated = javelin::tests::loadFixture("jmap/entities/mailbox.json");
        const auto unreadPosition = updated.find("\"unreadEmails\": 7");
        REQUIRE(unreadPosition != std::string::npos);
        updated.replace(unreadPosition, std::string{"\"unreadEmails\": 7"}.size(),
                        "\"unreadEmails\": 8");

        const auto unreadThreadsPosition = updated.find("\"unreadThreads\": 5");
        REQUIRE(unreadThreadsPosition != std::string::npos);
        updated.replace(unreadThreadsPosition, std::string{"\"unreadThreads\": 5"}.size(),
                        "\"unreadThreads\": 6");
        return updated;
    }

} // namespace

TEST_CASE("mailbox state refresh executor bootstraps mailbox metadata",
          "[jmap][sync][mailbox-state]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(serializeResponseEnvelope({
            .methodResponses =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Mailbox/get",
                        .arguments = mailboxGetArguments(
                            "mailbox-state-1",
                            javelin::tests::loadFixture("jmap/entities/mailbox.json")),
                        .callId = "mailboxes",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-1",
        })),
    });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxStateRefreshExecutor executor{databaseContext.connection,
                                                              methodCaller, makeRequestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxStateRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxStateRefreshSummary>(result);
    CHECK(summary.mailboxCount == 1);
    CHECK_FALSE(summary.usedIncrementalRefresh);

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto mailboxTreeResult = queryService.listMailboxTree("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
        mailboxTreeResult));
    const auto& mailboxTree =
        std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxTreeResult);
    REQUIRE(mailboxTree.size() == 1);
    CHECK(mailboxTree.front().id == "mbx-inbox");
    CHECK(mailboxTree.front().unreadEmails == 7);
}

TEST_CASE("mailbox state refresh executor applies mailbox changes", "[jmap][sync][mailbox-state]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::MailboxRepository mailboxRepository{databaseContext.connection};
    const auto parsedMailbox = javelin::jmap::domain::parseMailbox(
        javelin::tests::loadFixture("jmap/entities/mailbox.json"));
    REQUIRE(parsedMailbox.ok());
    REQUIRE(parsedMailbox.value.has_value());
    REQUIRE_FALSE(mailboxRepository.replaceAll("account-1", {*parsedMailbox.value}).has_value());

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1", .objectType = "Mailbox", .queryKey = {}},
                              "mailbox-state-1")
                      .has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(
            javelin::jmap::api::HttpResponse{
                .statusCode = 200,
                .body =
                    QByteArray::fromStdString(
                        serializeResponseEnvelope(
                            {
                                .methodResponses =
                                    {
                                        javelin::jmap::api::MethodInvocation{
                                            .name = "Mailbox/changes",
                                            .arguments =
                                                R"({"accountId":"account-1","oldState":"mailbox-state-1","newState":"mailbox-state-2","hasMoreChanges":false,"created":[],"updated":["mbx-inbox"],"destroyed":[]})",
                                            .callId = "mailbox-changes",
                                        },
                                    },
                                .createdIds = std::nullopt,
                                .sessionState = "session-state-2",
                            })),
            });
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(serializeResponseEnvelope({
            .methodResponses =
                {
                    javelin::jmap::api::MethodInvocation{
                        .name = "Mailbox/get",
                        .arguments =
                            mailboxGetArguments("mailbox-state-2", updatedMailboxFixture()),
                        .callId = "mailboxes",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxStateRefreshExecutor executor{databaseContext.connection,
                                                              methodCaller, makeRequestContext()};
    const auto result = QCoro::waitFor(executor.refresh("account-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxStateRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxStateRefreshSummary>(result);
    CHECK(summary.mailboxCount == 1);
    CHECK(summary.usedIncrementalRefresh);
    REQUIRE(transport.requests.size() == 2);

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto mailboxTreeResult = queryService.listMailboxTree("account-1");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MailboxTreeItem>>(
        mailboxTreeResult));
    const auto& mailboxTree =
        std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxTreeResult);
    REQUIRE(mailboxTree.size() == 1);
    CHECK(mailboxTree.front().unreadEmails == 8);
    CHECK(mailboxTree.front().unreadThreads == 6);
}
