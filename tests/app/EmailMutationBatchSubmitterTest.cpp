#include "app/EmailMutationBatchSubmitter.h"
#include "FixtureReader.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/EmailRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/domain/MailEntityParsers.h"
#include "jmap/sync/EmailMutationJournal.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
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
                return;
            static int argc = 1;
            static char appName[] = "email-mutation-batch-tests";
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

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(request);
            if (request.dispatched)
                request.dispatched();
            REQUIRE_FALSE(results.empty());
            auto result = std::move(results.front());
            results.erase(results.begin());
            co_return result;
        }

        javelin::jmap::api::HttpJmapMethodTransport methodTransport;
        std::vector<javelin::jmap::api::HttpRequest> requests;
        std::vector<javelin::jmap::api::TransportResult> results;
    };

    struct Fixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;
        FakeTransport transport;
        std::unique_ptr<javelin::jmap::EmailMutationEngine> mutationEngine;
    };

    [[nodiscard]] std::unique_ptr<Fixture> makeFixture(const std::uint64_t maxObjectsInSet = 2)
    {
        auto fixture = std::make_unique<Fixture>();
        REQUIRE(fixture->directory.isValid());
        static int connectionNumber = 0;
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = QStringLiteral("email-mutation-batches-%1").arg(++connectionNumber),
            .databasePath = fixture->directory.filePath(QStringLiteral("cache.sqlite3")),
        });
        REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
        fixture->database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

        const auto parsedSession = javelin::jmap::api::parseSession(
            javelin::tests::loadFixture("jmap/session/basic_session.json"),
            {.mail = true, .submission = false});
        REQUIRE(parsedSession.ok());
        REQUIRE(parsedSession.session.has_value());
        auto session = *parsedSession.session;
        REQUIRE(session.capabilities.coreDetails.has_value());
        session.capabilities.coreDetails->maxObjectsInSet = maxObjectsInSet;
        javelin::jmap::cache::SessionRepository sessions{fixture->database};
        REQUIRE_FALSE(sessions.replace("u1", session).has_value());

        const auto parsedEmail = javelin::jmap::domain::parseEmail(
            javelin::tests::loadFixture("jmap/entities/email.json"));
        REQUIRE(parsedEmail.ok());
        REQUIRE(parsedEmail.value.has_value());
        std::vector<javelin::jmap::domain::Email> emails;
        for (int index = 1; index <= 3; ++index)
        {
            auto email = *parsedEmail.value;
            email.id = "eml-" + std::to_string(index);
            email.threadId = "thr-1";
            email.keywords = {};
            emails.push_back(std::move(email));
        }
        javelin::jmap::cache::EmailRepository emailRepository{fixture->database};
        REQUIRE_FALSE(emailRepository.replaceAll("u1", emails).has_value());

        fixture->mutationEngine = std::make_unique<javelin::jmap::EmailMutationEngine>(
            fixture->database, fixture->transport.methodTransport);
        std::vector<javelin::jmap::EmailMailboxMutation> mutations;
        for (const auto& email : emails)
            mutations.push_back({.emailId = email.id,
                                 .addKeywords = {"$flagged"},
                                 .operationGroupId = "thread-command",
                                 .ifInState = "email-state-1"});
        REQUIRE(std::holds_alternative<std::vector<javelin::jmap::QueuedEmailMutation>>(
            fixture->mutationEngine->queueBatch("u1", std::move(mutations))));
        return fixture;
    }

    [[nodiscard]] javelin::jmap::LiveConnectionSettings settings()
    {
        return {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        };
    }
} // namespace

TEST_CASE("Email mutation operation groups submit one Email/set when they fit the server limit",
          "[app][email-mutation][batching][single-request]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto fixture = makeFixture(10);
    fixture->transport.results = {
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null,"eml-2":null,"eml-3":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
        },
    };

    javelin::app::EmailMutationBatchSubmitter submitter{*fixture->mutationEngine};
    const auto outcome = QCoro::waitFor(submitter.submit(settings(), "u1", "thread-command", 10));

    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.submitted.attemptedEmailCount == 3);
    CHECK(outcome.submitted.updatedEmailCount == 3);
    REQUIRE(fixture->transport.requests.size() == 1);
    CHECK(fixture->transport.requests.front().body.contains("eml-1"));
    CHECK(fixture->transport.requests.front().body.contains("eml-2"));
    CHECK(fixture->transport.requests.front().body.contains("eml-3"));
    CHECK(fixture->transport.requests.front().body.contains("\"Email/set\""));
}

TEST_CASE("Email mutation operation groups settle sequential bounded batches",
          "[app][email-mutation][batching]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto fixture = makeFixture();
    fixture->transport.results = {
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null,"eml-2":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
        },
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-2","newState":"email-state-2","updated":{},"notUpdated":{"eml-3":{"type":"forbidden"}}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-3"})",
        },
    };

    javelin::app::EmailMutationBatchSubmitter submitter{*fixture->mutationEngine};
    const auto outcome = QCoro::waitFor(submitter.submit(settings(), "u1", "thread-command", 2));

    CHECK_FALSE(outcome.error.has_value());
    CHECK(outcome.submitted.attemptedEmailCount == 3);
    CHECK(outcome.submitted.updatedEmailCount == 2);
    CHECK(outcome.submitted.failedEmailCount == 1);
    REQUIRE(fixture->transport.requests.size() == 2);
    CHECK(fixture->transport.requests[0].body.contains("\"ifInState\":\"email-state-1\""));
    CHECK(fixture->transport.requests[1].body.contains("\"ifInState\":\"email-state-2\""));
    CHECK_FALSE(fixture->transport.requests[0].body.contains("eml-3"));
    CHECK(fixture->transport.requests[1].body.contains("eml-3"));

    javelin::jmap::sync::EmailMutationJournal journal{fixture->database};
    const auto records = journal.listForOperationGroup("u1", "thread-command");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records));
    CHECK(std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records).empty());
}

TEST_CASE("Email mutation batching preserves accepted work across an ambiguous later batch",
          "[app][email-mutation][batching][ambiguity]")
{
    ApplicationGuard application;
    Q_UNUSED(application);
    auto fixture = makeFixture();
    fixture->transport.results = {
        javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body =
                R"({"methodResponses":[["Email/set",{"accountId":"u1","oldState":"email-state-1","newState":"email-state-2","updated":{"eml-1":null,"eml-2":null},"notUpdated":{}},"queued-email-set"]],"createdIds":{},"sessionState":"session-state-2"})",
        },
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "connection lost after dispatch",
        },
    };

    javelin::app::EmailMutationBatchSubmitter submitter{*fixture->mutationEngine};
    const auto outcome = QCoro::waitFor(submitter.submit(settings(), "u1", "thread-command", 2));

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.submitted.attemptedEmailCount == 2);
    CHECK(outcome.submitted.updatedEmailCount == 2);
    REQUIRE(fixture->transport.requests.size() == 2);
    CHECK(fixture->transport.requests[1].body.contains("\"ifInState\":\"email-state-2\""));

    javelin::jmap::sync::EmailMutationJournal journal{fixture->database};
    const auto records = journal.listForOperationGroup("u1", "thread-command");
    REQUIRE(std::holds_alternative<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records));
    const auto& remaining =
        std::get<std::vector<javelin::jmap::sync::EmailMutationRecord>>(records);
    REQUIRE(remaining.size() == 3);
    CHECK(remaining[0].patch.emailId == "eml-1");
    CHECK(remaining[0].status == javelin::jmap::sync::MutationStatus::Accepted);
    CHECK(remaining[1].patch.emailId == "eml-2");
    CHECK(remaining[1].status == javelin::jmap::sync::MutationStatus::Accepted);
    CHECK(remaining[2].patch.emailId == "eml-3");
    CHECK(remaining[2].status == javelin::jmap::sync::MutationStatus::Unknown);
    CHECK(remaining[2].baseState == std::optional<std::string>{"email-state-2"});
}
