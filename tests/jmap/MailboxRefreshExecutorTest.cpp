#include "jmap/sync/MailboxRefreshExecutor.h"
#include "FixtureReader.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/QueryService.h"
#include "jmap/cache/SyncStateRepository.h"
#include "jmap/cache/ThreadRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/sync/ConsistencyDomain.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
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
        FakeTransport() : methodTransport(*this)
        {
        }

        [[nodiscard]] operator javelin::jmap::api::JmapMethodTransport&()
        {
            return methodTransport;
        }

        javelin::jmap::api::HttpJmapMethodTransport methodTransport;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> queuedResults;
        std::function<void()> onSend;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            REQUIRE_FALSE(queuedResults.empty());

            auto result = std::move(queuedResults.front());
            queuedResults.erase(queuedResults.begin());
            if (onSend)
            {
                onSend();
            }
            co_return result;
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-mailbox-refresh-%1").arg(counter);
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

    [[nodiscard]] std::string mailboxQueryKey()
    {
        return javelin::jmap::sync::mailboxQueryKey({
            .mailboxId = "mbx-inbox",
            .sortProperty = "receivedAt",
            .isAscending = false,
            .collapseThreads = true,
        });
    }

    [[nodiscard]] std::string
    serializeResponseEnvelope(const javelin::jmap::api::ResponseEnvelope& envelope)
    {
        const auto serialized = javelin::jmap::api::serializeResponseEnvelope(envelope);
        REQUIRE(serialized.has_value());
        return *serialized;
    }

    [[nodiscard]] std::string updatedEmailFixture()
    {
        auto updated = javelin::tests::loadFixture("jmap/entities/email.json");
        const auto subjectPosition = updated.find("\"subject\": \"Quarterly update\"");
        REQUIRE(subjectPosition != std::string::npos);
        updated.replace(subjectPosition, std::string{"\"subject\": \"Quarterly update\""}.size(),
                        "\"subject\": \"Quarterly update v2\"");
        return updated;
    }

    [[nodiscard]] std::string newRepresentativeEmailFixture()
    {
        auto updated = javelin::tests::loadFixture("jmap/entities/email.json");
        const auto idPosition = updated.find("\"id\": \"eml-1\"");
        REQUIRE(idPosition != std::string::npos);
        updated.replace(idPosition, std::string{"\"id\": \"eml-1\""}.size(), "\"id\": \"eml-2\"");

        const auto receivedAtPosition = updated.find("\"receivedAt\": \"2026-04-05T11:22:33Z\"");
        REQUIRE(receivedAtPosition != std::string::npos);
        updated.replace(receivedAtPosition,
                        std::string{"\"receivedAt\": \"2026-04-05T11:22:33Z\""}.size(),
                        "\"receivedAt\": \"2026-04-06T12:22:33Z\"");

        const auto subjectPosition = updated.find("\"subject\": \"Quarterly update\"");
        REQUIRE(subjectPosition != std::string::npos);
        updated.replace(subjectPosition, std::string{"\"subject\": \"Quarterly update\""}.size(),
                        "\"subject\": \"New message in existing thread\"");

        const auto seenPosition = updated.find("\"$seen\": true");
        REQUIRE(seenPosition != std::string::npos);
        updated.replace(seenPosition, std::string{"\"$seen\": true"}.size(), "\"$seen\": false");
        return updated;
    }

    [[nodiscard]] javelin::jmap::domain::Email loadEmailFixture()
    {
        const auto parsed = javelin::jmap::domain::parseEmail(
            javelin::tests::loadFixture("jmap/entities/email.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

    [[nodiscard]] std::string emailGetArguments(std::string state, std::string emailJson)
    {
        return std::string{R"({"accountId":"account-1","state":")"} + std::move(state) +
               R"(","list":[)" + std::move(emailJson) + R"(],"notFound":[]})";
    }

    [[nodiscard]] std::string emailGetArguments(std::string state,
                                                const std::vector<std::string>& emailJsons)
    {
        std::string combined;
        for (std::size_t index = 0; index < emailJsons.size(); ++index)
        {
            if (index > 0)
            {
                combined += ",";
            }
            combined += emailJsons[index];
        }

        return emailGetArguments(std::move(state), std::move(combined));
    }

} // namespace

TEST_CASE("mailbox refresh executor bootstraps a collapsed mailbox into the cache",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    FakeTransport transport;
    transport
        .queuedResults.push_back(javelin::jmap::
                                     api::
                                         HttpResponse{
                                             .statusCode = 200,
                                             .body =
                                                 QByteArray::fromStdString(
                                                     serializeResponseEnvelope(
                                                         {
                                                             .methodResponses =
                                                                 {
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "Email/query",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                                                                             .callId =
                                                                                 "mailbox-query",
                                                                         },
                                                                     javelin::
                                                                         jmap::api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments =
                                                                                 emailGetArguments("email-state-1",
                                                                                                   javelin::tests::loadFixture("jmap/entities/email.json")),
                                                                             .callId =
                                                                                 "thread-ids-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Thread/get",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","state":"thread-state-1","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                                                                             .callId =
                                                                                 "threads-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments = emailGetArguments("email-state-1",
                                                                                                            javelin::
                                                                                                                tests::loadFixture(
                                                                                                                    "jmap/entities/email.json")),
                                                                             .callId = "mailbox-"
                                                                                       "emails-get",
                                                                         },
                                                                 },
                                                             .createdIds = std::nullopt,
                                                             .sessionState = "session-state-1",
                                                         })),
                                         });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
    CHECK(summary.representativeCount == 1);
    CHECK_FALSE(summary.usedIncrementalRefresh);
    CHECK(summary.changedEmailIds.empty());
    CHECK(summary.insertedEmailIds.empty());
    CHECK(summary.removedEmailIds.empty());
    CHECK_FALSE(summary.requiresNotificationScan);
    CHECK(summary.notificationCandidates.empty());
    REQUIRE(transport.requests.size() == 1);

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto listResult = queryService.listMailboxMessages("account-1", "mbx-inbox", 100);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(listResult));
    const auto& items = std::get<std::vector<javelin::jmap::cache::MessageListItem>>(listResult);
    REQUIRE(items.size() == 1);
    CHECK(items.front().emailId == "eml-1");
    CHECK(items.front().threadId == "thr-123");

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    const auto queryState = syncStateRepository.find({
        .accountId = "account-1",
        .objectType = "EmailQuery",
        .queryKey = mailboxQueryKey(),
    });
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(queryState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(queryState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(queryState)->stateToken ==
          "query-state-1");
}

TEST_CASE("mailbox refresh executor discards a response superseded by an accepted mutation",
          "[jmap][sync][refresh][consistency]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto archivedEmail = loadEmailFixture();
    archivedEmail.mailboxIds = {"mbx-archive"};
    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.upsertMany("account-1", {archivedEmail}).has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(
            javelin::jmap::api::HttpResponse{.statusCode = 200,
                                             .body =
                                                 QByteArray::fromStdString(
                                                     serializeResponseEnvelope(
                                                         {
                                                             .methodResponses =
                                                                 {
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "E"
                                                                                     "m"
                                                                                     "a"
                                                                                     "i"
                                                                                     "l"
                                                                                     "/"
                                                                                     "q"
                                                                                     "u"
                                                                                     "e"
                                                                                     "r"
                                                                                     "y",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","queryState":"stale-query-state","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                                                                             .callId = "m"
                                                                                       "a"
                                                                                       "i"
                                                                                       "l"
                                                                                       "b"
                                                                                       "o"
                                                                                       "x"
                                                                                       "-"
                                                                                       "q"
                                                                                       "u"
                                                                                       "e"
                                                                                       "r"
                                                                                       "y",
                                                                         },
                                                                     javelin::
                                                                         jmap::api::MethodInvocation{
                                                                             .name = "Ema"
                                                                                     "il/"
                                                                                     "ge"
                                                                                     "t",
                                                                             .arguments = emailGetArguments("stale-email-state",
                                                                                                            javelin::
                                                                                                                tests::loadFixture(
                                                                                                                    "jmap/entities/email.json")),
                                                                             .callId = "t"
                                                                                       "h"
                                                                                       "r"
                                                                                       "e"
                                                                                       "a"
                                                                                       "d"
                                                                                       "-"
                                                                                       "i"
                                                                                       "d"
                                                                                       "s"
                                                                                       "-"
                                                                                       "g"
                                                                                       "e"
                                                                                       "t",
                                                                         },
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "T"
                                                                                     "h"
                                                                                     "r"
                                                                                     "e"
                                                                                     "a"
                                                                                     "d"
                                                                                     "/"
                                                                                     "g"
                                                                                     "e"
                                                                                     "t",
                                                                             .arguments = R"({"accountId":"account-1","state":"thread-state","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                                                                             .callId = "t"
                                                                                       "h"
                                                                                       "r"
                                                                                       "e"
                                                                                       "a"
                                                                                       "d"
                                                                                       "s"
                                                                                       "-"
                                                                                       "g"
                                                                                       "e"
                                                                                       "t",
                                                                         },
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "Ema"
                                                                                     "il/"
                                                                                     "ge"
                                                                                     "t",
                                                                             .arguments = emailGetArguments("stale-email-state", javelin::
                                                                                                                                     tests::loadFixture(
                                                                                                                                         "jmap/entities/email.json")),
                                                                             .callId = "m"
                                                                                       "a"
                                                                                       "i"
                                                                                       "l"
                                                                                       "b"
                                                                                       "o"
                                                                                       "x"
                                                                                       "-"
                                                                                       "e"
                                                                                       "m"
                                                                                       "a"
                                                                                       "i"
                                                                                       "l"
                                                                                       "s"
                                                                                       "-"
                                                                                       "g"
                                                                                       "e"
                                                                                       "t",
                                                                         },
                                                                 },
                                                             .createdIds = std::nullopt,
                                                             .sessionState = "session-state",
                                                         }))});
    transport.onSend = [&databaseContext]
    {
        javelin::jmap::sync::ConsistencyDomainRepository consistencyRepository{
            databaseContext.connection};
        const auto generation = consistencyRepository.advanceMutation({
            .accountId = "account-1",
            .dataType = "Email",
        });
        REQUIRE(std::holds_alternative<std::uint64_t>(generation));
    };

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    CHECK(std::get<javelin::jmap::sync::MailboxRefreshSummary>(result).superseded);

    const auto emailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(emailResult));
    const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
    REQUIRE(email.has_value());
    CHECK(email->mailboxIds == std::vector<std::string>{"mbx-archive"});

    javelin::jmap::cache::SyncStateRepository states{databaseContext.connection};
    const auto queryState = states.find({
        .accountId = "account-1",
        .objectType = "EmailQuery",
        .queryKey = mailboxQueryKey(),
    });
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(queryState));
    CHECK_FALSE(
        std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(queryState).has_value());
}

TEST_CASE("mailbox refresh executor reapplies pending keyword mutations after a full refresh",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::sync::EmailMutationJournal emailMutationJournal{databaseContext.connection};
    REQUIRE_FALSE(emailMutationJournal
                      .put({
                          .mutationId = "action-unread",
                          .operationGroupId = std::nullopt,
                          .accountId = "account-1",
                          .status = javelin::jmap::sync::MutationStatus::Pending,
                          .patch =
                              {
                                  .emailId = "eml-1",
                                  .addMailboxIds = {},
                                  .removeMailboxIds = {},
                                  .addKeywords = {},
                                  .removeKeywords = {"$seen"},
                              },
                          .baseMailboxIds = std::nullopt,
                          .baseKeywords = std::nullopt,
                          .baseState = std::nullopt,
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());
    REQUIRE_FALSE(emailMutationJournal
                      .put({
                          .mutationId = "action-confirmed-flagged",
                          .operationGroupId = std::nullopt,
                          .accountId = "account-1",
                          .status = javelin::jmap::sync::MutationStatus::Unknown,
                          .patch =
                              {
                                  .emailId = "eml-1",
                                  .addMailboxIds = {},
                                  .removeMailboxIds = {},
                                  .addKeywords = {"$flagged"},
                                  .removeKeywords = {},
                              },
                          .baseMailboxIds = std::nullopt,
                          .baseKeywords = std::nullopt,
                          .baseState = std::nullopt,
                          .acceptedState = std::nullopt,
                          .errorJson = std::nullopt,
                      })
                      .has_value());

    FakeTransport transport;
    transport
        .queuedResults.push_back(javelin::jmap::
                                     api::
                                         HttpResponse{
                                             .statusCode = 200,
                                             .body =
                                                 QByteArray::fromStdString(
                                                     serializeResponseEnvelope(
                                                         {
                                                             .methodResponses =
                                                                 {
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "Email/query",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                                                                             .callId =
                                                                                 "mailbox-query",
                                                                         },
                                                                     javelin::
                                                                         jmap::api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments =
                                                                                 emailGetArguments("email-state-1",
                                                                                                   javelin::tests::loadFixture("jmap/entities/email.json")),
                                                                             .callId =
                                                                                 "thread-ids-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Thread/get",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","state":"thread-state-1","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                                                                             .callId =
                                                                                 "threads-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments = emailGetArguments("email-state-1",
                                                                                                            javelin::
                                                                                                                tests::loadFixture(
                                                                                                                    "jmap/entities/email.json")),
                                                                             .callId = "mailbox-"
                                                                                       "emails-get",
                                                                         },
                                                                 },
                                                             .createdIds = std::nullopt,
                                                             .sessionState = "session-state-1",
                                                         })),
                                         });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    const auto emailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(emailResult));
    const auto& email = std::get<std::optional<javelin::jmap::domain::Email>>(emailResult);
    REQUIRE(email.has_value());
    CHECK(std::find(email->keywords.cbegin(), email->keywords.cend(), std::string{"$seen"}) ==
          email->keywords.cend());
    const auto remaining = emailMutationJournal.listForEmail("account-1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(remaining));
    const auto& remainingRecords =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(remaining);
    REQUIRE(remainingRecords.size() == 1);
    CHECK(remainingRecords.front().mutationId == "action-unread");
}

TEST_CASE("mailbox refresh executor applies updated-only deltas without full rebuild",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    const auto parsedEmail =
        javelin::jmap::domain::parseEmail(javelin::tests::loadFixture("jmap/entities/email.json"));
    REQUIRE(parsedEmail.ok());
    REQUIRE(parsedEmail.value.has_value());
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {*parsedEmail.value}).has_value());

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1",
                               .objectType = "EmailQuery",
                               .queryKey = mailboxQueryKey()},
                              "query-state-1")
                      .has_value());
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                              "email-state-1")
                      .has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "Email/queryChanges",
                                                       .arguments =
                                                           R"({"accountId":"account-1","oldQueryState":"query-state-1","newQueryState":"query-state-2","added":[],"removed":[],"hasMoreChanges":false})",
                                                       .callId = "mailbox-query-changes",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email/changes",
                                                           .arguments =
                                                               R"({"accountId":"account-1","oldState":"email-state-1","newState":"email-state-2","hasMoreChanges":false,"created":[],"updated":["eml-1"],"destroyed":[]})",
                                                           .callId = "email-changes",
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
                        .name = "Email/get",
                        .arguments = emailGetArguments("email-state-2", updatedEmailFixture()),
                        .callId = "updated-emails-get",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
    CHECK(summary.representativeCount == 1);
    CHECK(summary.usedIncrementalRefresh);
    CHECK(summary.changedEmailIds == std::vector<std::string>{"eml-1"});
    CHECK(summary.insertedEmailIds.empty());
    CHECK(summary.removedEmailIds.empty());
    CHECK_FALSE(summary.requiresNotificationScan);
    CHECK(summary.notificationCandidates.empty());
    REQUIRE(transport.requests.size() == 2);

    const auto updatedEmailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(updatedEmailResult));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(updatedEmailResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(updatedEmailResult)->subject ==
          std::optional<std::string>{"Quarterly update v2"});
}

TEST_CASE("mailbox refresh executor rebuilds window when updated delta is missing locally",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1",
                               .objectType = "EmailQuery",
                               .queryKey = mailboxQueryKey()},
                              "query-state-1")
                      .has_value());
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                              "email-state-1")
                      .has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "Email/queryChanges",
                                                       .arguments =
                                                           R"({"accountId":"account-1","oldQueryState":"query-state-1","newQueryState":"query-state-2","added":[],"removed":[],"hasMoreChanges":false})",
                                                       .callId = "mailbox-query-changes",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email/changes",
                                                           .arguments =
                                                               R"({"accountId":"account-1","oldState":"email-state-1","newState":"email-state-2","hasMoreChanges":false,"created":[],"updated":["eml-1"],"destroyed":[]})",
                                                           .callId = "email-changes",
                                                       },
                                               },
                                           .createdIds = std::nullopt,
                                           .sessionState = "session-state-2",
                                       })),
                       });
    transport
        .queuedResults.push_back(javelin::jmap::
                                     api::
                                         HttpResponse{
                                             .statusCode = 200,
                                             .body =
                                                 QByteArray::fromStdString(
                                                     serializeResponseEnvelope(
                                                         {
                                                             .methodResponses =
                                                                 {
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "Email/query",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","queryState":"query-state-2","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                                                                             .callId =
                                                                                 "mailbox-query",
                                                                         },
                                                                     javelin::
                                                                         jmap::api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments =
                                                                                 emailGetArguments("email-state-2",
                                                                                                   javelin::tests::loadFixture("jmap/entities/email.json")),
                                                                             .callId =
                                                                                 "thread-ids-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Thread/get",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","state":"thread-state-2","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                                                                             .callId =
                                                                                 "threads-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments = emailGetArguments("email-state-2",
                                                                                                            javelin::
                                                                                                                tests::loadFixture(
                                                                                                                    "jmap/entities/email.json")),
                                                                             .callId = "mailbox-"
                                                                                       "emails-get",
                                                                         },
                                                                 },
                                                             .createdIds = std::nullopt,
                                                             .sessionState = "session-state-2",
                                                         })),
                                         });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
    CHECK_FALSE(summary.usedIncrementalRefresh);
    CHECK(summary.changedEmailIds == std::vector<std::string>{"eml-1"});
    CHECK(summary.insertedEmailIds == std::vector<std::string>{"eml-1"});
    REQUIRE(transport.requests.size() == 2);

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    const auto emailResult = emailRepository.find("account-1", "eml-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(emailResult));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(emailResult).has_value());
}

TEST_CASE("mailbox refresh executor preserves change hints when delta falls back to full fetch",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto originalEmail = loadEmailFixture();
    originalEmail.id = "eml-1";
    originalEmail.mailboxIds = {"mbx-inbox"};
    auto removedEmail = originalEmail;
    removedEmail.id = "eml-removed";
    removedEmail.threadId = "thr-removed";
    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(
        emailRepository.replaceAll("account-1", {originalEmail, removedEmail}).has_value());

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1",
                               .objectType = "EmailQuery",
                               .queryKey = mailboxQueryKey()},
                              "query-state-1")
                      .has_value());
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                              "email-state-1")
                      .has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "Email/queryChanges",
                                                       .arguments =
                                                           R"({"accountId":"account-1","oldQueryState":"query-state-1","newQueryState":"query-state-2","added":[{"id":"eml-new","index":0}],"removed":["eml-removed"],"hasMoreChanges":false})",
                                                       .callId = "mailbox-query-changes",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email/changes",
                                                           .arguments =
                                                               R"({"accountId":"account-1","oldState":"email-state-1","newState":"email-state-2","hasMoreChanges":false,"created":["eml-new","eml-created-only"],"updated":["eml-updated"],"destroyed":["eml-removed"]})",
                                                           .callId = "email-changes",
                                                       },
                                               },
                                           .createdIds = std::nullopt,
                                           .sessionState = "session-state-2",
                                       })),
                       });
    transport
        .queuedResults.push_back(javelin::jmap::
                                     api::
                                         HttpResponse{
                                             .statusCode = 200,
                                             .body =
                                                 QByteArray::fromStdString(
                                                     serializeResponseEnvelope(
                                                         {
                                                             .methodResponses =
                                                                 {
                                                                     javelin::jmap::api::
                                                                         MethodInvocation{
                                                                             .name = "Email/query",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","queryState":"query-state-2","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":500})",
                                                                             .callId =
                                                                                 "mailbox-query",
                                                                         },
                                                                     javelin::
                                                                         jmap::api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments =
                                                                                 emailGetArguments("email-state-2",
                                                                                                   javelin::tests::loadFixture("jmap/entities/email.json")),
                                                                             .callId =
                                                                                 "thread-ids-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Thread/get",
                                                                             .arguments =
                                                                                 R"({"accountId":"account-1","state":"thread-state-2","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                                                                             .callId =
                                                                                 "threads-get",
                                                                         },
                                                                     javelin::jmap::
                                                                         api::MethodInvocation{
                                                                             .name = "Email/get",
                                                                             .arguments = emailGetArguments("email-state-2",
                                                                                                            javelin::
                                                                                                                tests::loadFixture(
                                                                                                                    "jmap/entities/email.json")),
                                                                             .callId = "mailbox-"
                                                                                       "emails-get",
                                                                         },
                                                                 },
                                                             .createdIds = std::nullopt,
                                                             .sessionState = "session-state-2",
                                                         })),
                                         });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
    CHECK(summary.representativeCount == 1);
    CHECK_FALSE(summary.usedIncrementalRefresh);
    CHECK(summary.changedEmailIds == std::vector<std::string>{"eml-updated"});
    CHECK(summary.insertedEmailIds == std::vector<std::string>{"eml-new", "eml-created-only"});
    CHECK(summary.removedEmailIds == std::vector<std::string>{"eml-removed"});
    CHECK(summary.requiresNotificationScan);
    CHECK(summary.notificationCandidates.empty());
    REQUIRE(transport.requests.size() == 2);
    const auto removedResult = emailRepository.find("account-1", "eml-removed");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(removedResult));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::domain::Email>>(removedResult).has_value());
}

TEST_CASE("mailbox refresh executor derives inserted email ids from full fetch fallback",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto originalEmail = loadEmailFixture();
    originalEmail.id = "eml-1";
    originalEmail.threadId = "thr-123";
    originalEmail.mailboxIds = {"mbx-inbox"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {originalEmail}).has_value());

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1",
                               .objectType = "EmailQuery",
                               .queryKey = mailboxQueryKey()},
                              "query-state-1")
                      .has_value());
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                              "email-state-1")
                      .has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "error",
                                                       .arguments =
                                                           R"({"type":"cannotCalculateChanges","description":"delta unavailable"})",
                                                       .callId = "mailbox-query-changes",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email/changes",
                                                           .arguments =
                                                               R"({"accountId":"account-1","oldState":"email-state-1","newState":"email-state-2","hasMoreChanges":false,"created":["eml-2"],"updated":[],"destroyed":[]})",
                                                           .callId = "email-changes",
                                                       },
                                               },
                                           .createdIds = std::nullopt,
                                           .sessionState = "session-state-2",
                                       })),
                       });
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "Email/query",
                                                       .arguments =
                                                           R"({"accountId":"account-1","queryState":"query-state-2","canCalculateChanges":true,"position":0,"ids":["eml-2"],"total":1})",
                                                       .callId = "mailbox"
                                                                 "-query",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email"
                                                                   "/get",
                                                           .arguments =
                                                               emailGetArguments("email"
                                                                                 "-stat"
                                                                                 "e-2",
                                                                                 {newRepresentativeEmailFixture()}),
                                                           .callId = "thread-"
                                                                     "ids-get",
                                                       },
                                                   javelin::
                                                       jmap::
                                                           api::MethodInvocation{
                                                               .name = "Thread/get",
                                                               .arguments =
                                                                   R"({"accountId":"account-1","state":"thread-state-2","list":[{"id":"thr-123","emailIds":["eml-2","eml-1"]}],"notFound":[]})",
                                                               .callId = "threads-get",
                                                           },
                                                   javelin::
                                                       jmap::
                                                           api::MethodInvocation{
                                                               .name = "Email"
                                                                       "/get",
                                                               .arguments =
                                                                   emailGetArguments("email"
                                                                                     "-stat"
                                                                                     "e-2",
                                                                                     {javelin::
                                                                                          tests::loadFixture("jmap/entities/email.json"),
                                                                                      newRepresentativeEmailFixture()}),
                                                               .callId = "mailbox-"
                                                                         "emails-"
                                                                         "get",
                                                           },
                                               },
                                           .createdIds = std::nullopt,
                                           .sessionState = "session-state-2",
                                       })),
                       });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
    CHECK_FALSE(summary.usedIncrementalRefresh);
    CHECK(summary.insertedEmailIds == std::vector<std::string>{"eml-2"});
    CHECK(summary.removedEmailIds.empty());
    CHECK(summary.requiresNotificationScan);
    REQUIRE(summary.notificationCandidates.size() == 1);
    CHECK(summary.notificationCandidates.front().emailId == "eml-2");
    CHECK(summary.notificationCandidates.front().threadId == "thr-123");
}

TEST_CASE("mailbox refresh executor full fallback preserves unrelated account cache rows",
          "[jmap][sync][refresh]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    seedAccount(databaseContext.connection);

    auto inboxEmail = loadEmailFixture();
    inboxEmail.id = "eml-1";
    inboxEmail.threadId = "thr-inbox";
    inboxEmail.mailboxIds = {"mbx-inbox"};

    auto archivedEmail = loadEmailFixture();
    archivedEmail.id = "eml-archive";
    archivedEmail.threadId = "thr-archive";
    archivedEmail.mailboxIds = {"mbx-archive"};

    javelin::jmap::cache::EmailRepository emailRepository{databaseContext.connection};
    REQUIRE_FALSE(emailRepository.replaceAll("account-1", {inboxEmail, archivedEmail}).has_value());

    javelin::jmap::cache::ThreadRepository threadRepository{databaseContext.connection};
    REQUIRE_FALSE(threadRepository
                      .replaceAll("account-1",
                                  {
                                      javelin::jmap::domain::Thread{
                                          .id = "thr-inbox",
                                          .emailIds = {"eml-1"},
                                      },
                                      javelin::jmap::domain::Thread{
                                          .id = "thr-archive",
                                          .emailIds = {"eml-archive"},
                                      },
                                  })
                      .has_value());

    javelin::jmap::cache::SyncStateRepository syncStateRepository{databaseContext.connection};
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1",
                               .objectType = "EmailQuery",
                               .queryKey = mailboxQueryKey()},
                              "query-state-1")
                      .has_value());
    REQUIRE_FALSE(syncStateRepository
                      .upsert({.accountId = "account-1", .objectType = "Email", .queryKey = {}},
                              "email-state-1")
                      .has_value());

    FakeTransport transport;
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "error",
                                                       .arguments =
                                                           R"({"type":"cannotCalculateChanges","description":"delta unavailable"})",
                                                       .callId = "mailbox-query-changes",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email/changes",
                                                           .arguments =
                                                               R"({"accountId":"account-1","oldState":"email-state-1","newState":"email-state-2","hasMoreChanges":false,"created":["eml-2"],"updated":[],"destroyed":[]})",
                                                           .callId = "email-changes",
                                                       },
                                               },
                                           .createdIds = std::nullopt,
                                           .sessionState = "session-state-2",
                                       })),
                       });
    transport.queuedResults
        .push_back(javelin::jmap::api::
                       HttpResponse{
                           .statusCode = 200,
                           .body =
                               QByteArray::fromStdString(
                                   serializeResponseEnvelope(
                                       {
                                           .methodResponses =
                                               {
                                                   javelin::jmap::api::MethodInvocation{
                                                       .name = "Email/query",
                                                       .arguments =
                                                           R"({"accountId":"account-1","queryState":"query-state-2","canCalculateChanges":true,"position":0,"ids":["eml-2"],"total":1})",
                                                       .callId = "mailbox-query",
                                                   },
                                                   javelin::
                                                       jmap::api::MethodInvocation{
                                                           .name = "Email/get",
                                                           .arguments =
                                                               emailGetArguments("email-state-2",
                                                                                 {newRepresentativeEmailFixture()}),
                                                           .callId = "thread-ids-get",
                                                       },
                                                   javelin::
                                                       jmap::
                                                           api::MethodInvocation{
                                                               .name = "Thread/get",
                                                               .arguments =
                                                                   R"({"accountId":"account-1","state":"thread-state-2","list":[{"id":"thr-123","emailIds":["eml-2"]}],"notFound":[]})",
                                                               .callId = "threads-get",
                                                           },
                                                   javelin::
                                                       jmap::
                                                           api::MethodInvocation{
                                                               .name = "Email/get",
                                                               .arguments =
                                                                   emailGetArguments(
                                                                       "email-state-2", {newRepresentativeEmailFixture()}),
                                                               .callId = "mailbox-emails-get",
                                                           },
                                               },
                                           .createdIds = std::nullopt,
                                           .sessionState = "session-state-2",
                                       })),
                       });

    javelin::jmap::api::MethodCaller methodCaller{transport};
    javelin::jmap::sync::MailboxRefreshExecutor executor{databaseContext.connection, methodCaller,
                                                         makeRequestContext()};
    const auto result =
        QCoro::waitFor(executor.refreshCollapsedMailbox("account-1", "mbx-inbox", {}));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::MailboxRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::sync::MailboxRefreshSummary>(result);
    CHECK_FALSE(summary.usedIncrementalRefresh);
    CHECK(summary.insertedEmailIds == std::vector<std::string>{"eml-2"});
    CHECK(summary.removedEmailIds == std::vector<std::string>{"eml-1"});
    CHECK(summary.requiresNotificationScan);

    javelin::jmap::cache::QueryService queryService{databaseContext.connection};
    const auto inboxListResult = queryService.listMailboxMessages("account-1", "mbx-inbox", 100);
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::cache::MessageListItem>>(
        inboxListResult));
    const auto& inboxItems =
        std::get<std::vector<javelin::jmap::cache::MessageListItem>>(inboxListResult);
    REQUIRE(inboxItems.size() == 1);
    CHECK(inboxItems.front().emailId == "eml-2");

    const auto archivedEmailResult = emailRepository.find("account-1", "eml-archive");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Email>>(archivedEmailResult));
    REQUIRE(std::get<std::optional<javelin::jmap::domain::Email>>(archivedEmailResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Email>>(archivedEmailResult)->mailboxIds ==
          std::vector<std::string>{"mbx-archive"});

    const auto archivedThreadResult = threadRepository.find("account-1", "thr-archive");
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::domain::Thread>>(archivedThreadResult));
    REQUIRE(
        std::get<std::optional<javelin::jmap::domain::Thread>>(archivedThreadResult).has_value());
    CHECK(std::get<std::optional<javelin::jmap::domain::Thread>>(archivedThreadResult)->emailIds ==
          std::vector<std::string>{"eml-archive"});
}
