#include "app/ThreadMembershipMaterializationWorker.h"
#include "app/AccountConnectionProvider.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MailMethods.h"
#include "jmap/api/RequestBuilder.h"
#include "jmap/api/Session.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/ThreadRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;
        static int argc = 1;
        static char name[] = "thread-membership-materialization-worker-test";
        static char* argv[]{name, nullptr};
        static const auto application = std::make_unique<QCoreApplication>(argc, argv);
        Q_UNUSED(application);
    }

    [[nodiscard]] QString connectionName()
    {
        static int counter = 0;
        return QStringLiteral("thread-membership-worker-%1").arg(++counter);
    }

    struct Fixture
    {
        QTemporaryDir directory;
        javelin::jmap::cache::DatabaseConnection database;

        Fixture()
        {
            REQUIRE(directory.isValid());
            auto opened = javelin::jmap::cache::DatabaseConnection::open({
                .connectionName = connectionName(),
                .databasePath = directory.filePath(QStringLiteral("cache.sqlite3")),
            });
            REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
            database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

            QSqlQuery account{database.database()};
            REQUIRE(account.exec(QStringLiteral(
                "INSERT INTO accounts(account_id,email_address,session_url,is_primary) VALUES("
                "'account-1','alice@example.test','https://example.test/session',1)")));
            javelin::jmap::cache::SessionRepository sessions{database};
            REQUIRE_FALSE(sessions.replace("account-1", session()).has_value());
        }

        static javelin::jmap::api::Session session()
        {
            javelin::jmap::api::Session value{
                .username = "alice@example.test",
                .apiUrl = "https://example.test/jmap",
                .downloadUrl = "https://example.test/download/{accountId}/{blobId}/{name}",
                .uploadUrl = "https://example.test/upload/{accountId}",
                .eventSourceUrl = std::nullopt,
                .state = "session-state",
                .capabilities =
                    {
                        .core = true,
                        .coreDetails =
                            javelin::jmap::api::CoreCapability{
                                .maxSizeUpload = std::nullopt,
                                .maxConcurrentUpload = std::nullopt,
                                .maxSizeRequest = 1024 * 1024,
                                .maxConcurrentRequests = 4,
                                .maxCallsInRequest = 4,
                                .maxObjectsInGet = 2,
                                .maxObjectsInSet = 2,
                                .collationAlgorithms = {},
                            },
                        .mail = true,
                        .submission = false,
                        .contacts = false,
                        .calendars = false,
                        .sieve = false,
                        .websocket = std::nullopt,
                    },
                .accounts = {},
                .primaryAccounts = {.mailAccountId = "account-1",
                                    .submissionAccountId = std::nullopt,
                                    .contactsAccountId = std::nullopt,
                                    .calendarsAccountId = std::nullopt,
                                    .sieveAccountId = std::nullopt},
            };
            value.accounts.emplace("account-1", javelin::jmap::api::Account{
                                                    .id = "account-1",
                                                    .name = "Test",
                                                    .isPersonal = true,
                                                    .isReadOnly = false,
                                                    .accountCapabilities =
                                                        {
                                                            .mail = true,
                                                            .mailDetails = std::nullopt,
                                                            .submission = std::nullopt,
                                                            .contacts = std::nullopt,
                                                            .calendars = std::nullopt,
                                                            .sieve = false,
                                                        },
                                                });
            return value;
        }

        void seedRepresentative(const std::string& threadId)
        {
            QSqlQuery email{database.database()};
            email.prepare(QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id) "
                                         "VALUES('account-1',:email,:thread)"));
            email.bindValue(QStringLiteral(":email"),
                            QString::fromStdString(std::string{"representative-"} + threadId));
            email.bindValue(QStringLiteral(":thread"), QString::fromStdString(threadId));
            REQUIRE(email.exec());
        }
    };

    class ConnectionProvider final : public javelin::app::AccountConnectionProvider
    {
      public:
        [[nodiscard]] std::optional<javelin::app::AccountConnectionSettings>
        connectionSettingsFor(std::string_view ownerAccountId) const override
        {
            if (ownerAccountId != "account-1")
                return std::nullopt;
            return javelin::app::AccountConnectionSettings{
                .connectionId = "connection-1",
                .revision = 1,
                .displayName = "Test",
                .sessionUrl = "https://example.test/session",
                .loginEmail = "alice@example.test",
                .apiKey = "access-token",
                .refreshToken = {},
                .tokenEndpoint = {},
                .oauthClientId = {},
                .oauthIssuer = {},
                .oauthResource = {},
                .oauthScope = {},
                .revocationEndpoint = {},
                .registrationClientUri = {},
                .registrationAccessToken = {},
            };
        }
    };

    [[nodiscard]] std::string threadArguments(const std::vector<std::string>& ids,
                                              const std::vector<std::string>& notFound = {},
                                              const bool includeChild = true,
                                              const std::size_t childCount = 1)
    {
        std::string list;
        for (const auto& id : ids)
        {
            if (!list.empty())
                list += ',';
            std::string emailIds = R"("representative-)" + id + '"';
            if (includeChild)
            {
                for (std::size_t child = 1; child <= childCount; ++child)
                {
                    emailIds += R"(,"child-)" + id;
                    if (childCount > 1)
                        emailIds += '-' + std::to_string(child);
                    emailIds += '"';
                }
            }
            list += R"({"id":")" + id + R"(","emailIds":[)" + emailIds + "]}";
        }
        std::string missing;
        for (const auto& id : notFound)
        {
            if (!missing.empty())
                missing += ',';
            missing += '"' + id + '"';
        }
        return R"({"accountId":"account-1","state":"thread-state","list":[)" + list +
               R"(],"notFound":[)" + missing + "]}";
    }

    [[nodiscard]] std::string emailArguments(const std::vector<std::string>& ids,
                                             const std::vector<std::string>& notFound = {})
    {
        std::string list;
        for (const auto& id : ids)
        {
            if (!list.empty())
                list += ',';
            auto threadId = id.substr(std::string{"child-"}.size());
            if (const auto secondSeparator = threadId.find('-', threadId.find('-') + 1);
                secondSeparator != std::string::npos)
                threadId.erase(secondSeparator);
            list +=
                R"({"id":")" + id + R"(","blobId":"blob-)" + id + R"(","threadId":")" + threadId +
                R"(","mailboxIds":{},"keywords":{},"size":100,"receivedAt":"2026-08-11T00:00:00Z","hasAttachment":false,"subject":")" +
                id + R"(","from":[],"to":[],"cc":[],"bcc":[],"replyTo":[],"preview":"child"})";
        }
        std::string missing;
        for (const auto& id : notFound)
        {
            if (!missing.empty())
                missing += ',';
            missing += '"' + id + '"';
        }
        return R"({"accountId":"account-1","state":"email-state","list":[)" + list +
               R"(],"notFound":[)" + missing + "]}";
    }

    class RecordingTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::vector<std::vector<std::string>> batches;
        std::vector<std::vector<std::string>> emailBatches;
        std::optional<std::string> threadNotFound;
        std::optional<std::string> emailNotFound;
        bool reconcileWithoutMissingChild = false;
        bool omitLast = false;
        std::size_t childCount = 1;
        std::unordered_map<std::string, std::size_t> threadRequests;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            REQUIRE(request.envelope.methodCalls.size() == 1);
            const auto& method = request.envelope.methodCalls.front();

            std::vector<std::string> ids;
            for (std::size_t number = 1; number <= 5; ++number)
            {
                const auto id = std::string{"thread-"} + std::to_string(number);
                const auto requestedId =
                    method.name == "Email/get" ? std::string{"child-"} + id : id;
                if (method.name == "Email/get" && childCount > 1)
                {
                    for (std::size_t child = 1; child <= childCount; ++child)
                    {
                        const auto childId = requestedId + '-' + std::to_string(child);
                        if (method.arguments.contains('"' + childId + '"'))
                            ids.push_back(childId);
                    }
                }
                else if (method.arguments.contains('"' + requestedId + '"'))
                    ids.push_back(requestedId);
            }
            REQUIRE(ids.size() <= 2);
            if (method.name == "Email/get")
            {
                emailBatches.push_back(ids);
                std::vector<std::string> found;
                std::vector<std::string> missing;
                for (const auto& id : ids)
                {
                    if (emailNotFound == id)
                        missing.push_back(id);
                    else
                        found.push_back(id);
                }
                co_return javelin::jmap::api::ResponseEnvelope{
                    .methodResponses = {{
                        .name = "Email/get",
                        .arguments = emailArguments(found, missing),
                        .callId = method.callId,
                    }},
                    .createdIds = std::nullopt,
                    .sessionState = "session-state",
                };
            }

            REQUIRE(method.name == "Thread/get");
            batches.push_back(ids);
            std::vector<std::string> found;
            std::vector<std::string> missing;
            for (const auto& id : ids)
            {
                if (threadNotFound == id)
                    missing.push_back(id);
                else
                    found.push_back(id);
            }
            if (omitLast && !found.empty())
                found.pop_back();
            bool includeChild = true;
            if (reconcileWithoutMissingChild && ids.size() == 1 && threadRequests[ids.front()] > 0)
                includeChild = false;
            for (const auto& id : ids)
                ++threadRequests[id];
            co_return javelin::jmap::api::ResponseEnvelope{
                .methodResponses = {{
                    .name = "Thread/get",
                    .arguments = threadArguments(found, missing, includeChild, childCount),
                    .callId = method.callId,
                }},
                .createdIds = std::nullopt,
                .sessionState = "session-state",
            };
        }
    };
} // namespace

TEST_CASE("Thread materialization hydrates explicit bounded child Email batches",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    for (std::size_t number = 1; number <= 5; ++number)
        fixture.seedRepresentative(std::string{"thread-"} + std::to_string(number));
    RecordingTransport transport;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};

    std::vector<std::pair<quint64, quint64>> progress;
    QObject::connect(&worker, &javelin::app::ThreadMembershipMaterializationWorker::progressChanged,
                     [&progress](const QString&, const quint64 completed, const quint64 total)
                     { progress.emplace_back(completed, total); });
    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-5", "thread-3", "thread-1", "thread-4", "thread-2"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    const auto& summary = std::get<javelin::app::ThreadMaterializationSummary>(result);
    CHECK(summary.completedThreadCount == 5);
    CHECK(summary.completedEmailCount == 5);
    CHECK(summary.missingEmailIds.empty());
    REQUIRE(transport.batches.size() == 3);
    CHECK(transport.batches[0] == std::vector<std::string>{"thread-3", "thread-5"});
    CHECK(transport.batches[1] == std::vector<std::string>{"thread-1", "thread-4"});
    CHECK(transport.batches[2] == std::vector<std::string>{"thread-2"});
    REQUIRE(transport.emailBatches.size() == 5);
    CHECK(std::ranges::all_of(transport.emailBatches,
                              [](const auto& batch) { return batch.size() == 1; }));
    CHECK(progress == std::vector<std::pair<quint64, quint64>>{{2, 5}, {4, 5}, {5, 5}});

    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    const auto membership = threads.findMembership("account-1", "thread-3");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membership));
    const auto& record =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership);
    REQUIRE(record.has_value());
    CHECK(record->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Current);
    CHECK(record->thread.emailIds ==
          std::vector<std::string>{"representative-thread-3", "child-thread-3"});
    CHECK(record->state == "thread-state");
    const auto coverage = threads.coverage("account-1", "thread-3");
    const auto& covered = std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage);
    REQUIRE(covered.has_value());
    CHECK(covered->childEmailsComplete);
}

TEST_CASE("current Thread membership cardinality mismatch is repaired from the server",
          "[app][thread-materialization][recovery]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1",
                                  {{.id = "thread-1",
                                    .emailIds = {"representative-thread-1", "child-thread-1"}}},
                                  "thread-state")
                      .has_value());
    QSqlQuery corrupt{fixture.database.database()};
    REQUIRE(corrupt.exec(
        QStringLiteral("DELETE FROM thread_email_members WHERE account_id='account-1' AND "
                       "thread_id='thread-1' AND email_id='child-thread-1'")));

    RecordingTransport transport;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};
    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    REQUIRE(transport.batches.size() == 1);
    CHECK(transport.batches.front() == std::vector<std::string>{"thread-1"});
    const auto coverage = threads.coverage("account-1", "thread-1");
    const auto& repaired = std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage);
    REQUIRE(repaired.has_value());
    CHECK(repaired->childEmailsComplete);
}

TEST_CASE("cached same-Thread Email outside membership is repaired from the server",
          "[app][thread-materialization][recovery]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    QSqlQuery cachedReply{fixture.database.database()};
    REQUIRE(cachedReply.exec(QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id) "
                                            "VALUES('account-1','child-thread-1','thread-1')")));
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1",
                                  {{.id = "thread-1", .emailIds = {"representative-thread-1"}}},
                                  "old-thread-state")
                      .has_value());

    RecordingTransport transport;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};
    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    REQUIRE(transport.batches.size() == 1);
    CHECK(transport.batches.front() == std::vector<std::string>{"thread-1"});
    CHECK(transport.emailBatches.empty());
    const auto coverage = threads.coverage("account-1", "thread-1");
    const auto& repaired = std::get<std::optional<javelin::jmap::cache::ThreadCoverage>>(coverage);
    REQUIRE(repaired.has_value());
    CHECK(repaired->globalMemberCount == 2);
    CHECK(repaired->untrackedCachedEmailCount == 0);
    CHECK(repaired->childEmailsComplete);
}

TEST_CASE("represented Thread notFound remains a reconciliation failure",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    fixture.seedRepresentative("thread-2");
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(
        threads
            .upsertMany("account-1", {{.id = "thread-2", .emailIds = {"old-child"}}}, "old-state")
            .has_value());
    REQUIRE_FALSE(threads.markStale("account-1", std::array{std::string{"thread-2"}}).has_value());

    RecordingTransport transport;
    transport.threadNotFound = "thread-2";
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};
    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1", "thread-2"},
    }));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::NotFound);
    const auto found = threads.findMembership("account-1", "thread-1");
    REQUIRE(
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(found).has_value());
    const auto missing = threads.findMembership("account-1", "thread-2");
    const auto& stale =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(missing);
    REQUIRE(stale.has_value());
    CHECK(stale->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Stale);
    CHECK(stale->thread.emailIds == std::vector<std::string>{"old-child"});
}

TEST_CASE("pending Email summary refresh survives a completed Thread membership",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    QSqlQuery seed{fixture.database.database()};
    REQUIRE(
        seed.exec(QStringLiteral("INSERT INTO emails(account_id,email_id,thread_id,subject) VALUES"
                                 "('account-1','child-thread-1','thread-1','stale child')")));
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1",
                                  {{.id = "thread-1",
                                    .emailIds = {"representative-thread-1", "child-thread-1"}}},
                                  "thread-state")
                      .has_value());
    REQUIRE(
        seed.exec(QStringLiteral("INSERT INTO email_summary_refresh_requests(account_id,email_id) "
                                 "VALUES('account-1','child-thread-1')")));

    RecordingTransport transport;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};
    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    CHECK(std::get<javelin::app::ThreadMaterializationSummary>(result).completedEmailCount == 1);
    CHECK(transport.batches.empty());
    REQUIRE(transport.emailBatches.size() == 1);
    CHECK(transport.emailBatches.front() == std::vector<std::string>{"child-thread-1"});
    REQUIRE(seed.exec(QStringLiteral("SELECT COUNT(*) FROM email_summary_refresh_requests WHERE "
                                     "account_id='account-1' AND email_id='child-thread-1'")));
    REQUIRE(seed.next());
    CHECK(seed.value(0).toInt() == 0);
}

TEST_CASE("one pathological Thread hydrates child Emails without exceeding get limits",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    RecordingTransport transport;
    transport.childCount = 5;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};

    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    CHECK(std::get<javelin::app::ThreadMaterializationSummary>(result).completedEmailCount == 5);
    REQUIRE(transport.emailBatches.size() == 3);
    CHECK(transport.emailBatches[0].size() == 2);
    CHECK(transport.emailBatches[1].size() == 2);
    CHECK(transport.emailBatches[2].size() == 1);
}

TEST_CASE("Thread membership materialization rejects incomplete response accounting",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    fixture.seedRepresentative("thread-2");
    RecordingTransport transport;
    transport.omitLast = true;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};

    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1", "thread-2"},
    }));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::ProtocolViolation);
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    const auto membership = threads.findMembership("account-1", "thread-1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(
        membership));
    CHECK_FALSE(std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership)
                    .has_value());
}

TEST_CASE("Thread batches shrink to the negotiated request byte limit",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    fixture.seedRepresentative("thread-2");
    const auto oneRequest = javelin::jmap::api::threadGet({
        .accountId = "account-1",
        .ids = std::vector<std::string>{"thread-1"},
        .idsReference = std::nullopt,
        .properties = std::nullopt,
    });
    REQUIRE(oneRequest.has_value());
    javelin::jmap::api::RequestBuilder builder;
    builder.useCore().useMail();
    static_cast<void>(builder.call(*oneRequest, "thread-membership"));
    const auto encoded = javelin::jmap::api::serializeRequestEnvelope(builder.build());
    REQUIRE(encoded.has_value());
    auto session = Fixture::session();
    session.capabilities.coreDetails->maxSizeRequest = encoded->size();
    javelin::jmap::cache::SessionRepository sessions{fixture.database};
    REQUIRE_FALSE(sessions.replace("account-1", session).has_value());

    RecordingTransport transport;
    transport.childCount = 0;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};
    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1", "thread-2"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    REQUIRE(transport.batches.size() == 2);
    CHECK(transport.batches[0].size() == 1);
    CHECK(transport.batches[1].size() == 1);
    CHECK(transport.emailBatches.empty());
}

TEST_CASE("child Email batches shrink to the negotiated request byte limit",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    const std::vector<std::string> properties{
        "id",     "blobId",    "threadId",  "mailboxIds", "keywords",      "size",    "receivedAt",
        "sentAt", "messageId", "inReplyTo", "references", "hasAttachment", "subject", "from",
        "to",     "cc",        "bcc",       "replyTo",    "preview",
    };
    const auto oneRequest = javelin::jmap::api::emailGet({
        .accountId = "account-1",
        .ids = std::vector<std::string>{"child-thread-1-1"},
        .idsReference = std::nullopt,
        .properties = properties,
    });
    REQUIRE(oneRequest.has_value());
    javelin::jmap::api::RequestBuilder builder;
    builder.useCore().useMail();
    static_cast<void>(builder.call(*oneRequest, "thread-child-emails"));
    const auto encoded = javelin::jmap::api::serializeRequestEnvelope(builder.build());
    REQUIRE(encoded.has_value());
    auto session = Fixture::session();
    session.capabilities.coreDetails->maxSizeRequest = encoded->size();
    javelin::jmap::cache::SessionRepository sessions{fixture.database};
    REQUIRE_FALSE(sessions.replace("account-1", session).has_value());

    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    REQUIRE_FALSE(threads
                      .upsertMany("account-1",
                                  {{.id = "thread-1",
                                    .emailIds = {"representative-thread-1", "child-thread-1-1",
                                                 "child-thread-1-2"}}},
                                  "thread-state")
                      .has_value());
    RecordingTransport transport;
    transport.childCount = 2;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};

    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    CHECK(transport.batches.empty());
    REQUIRE(transport.emailBatches.size() == 2);
    CHECK(transport.emailBatches[0].size() == 1);
    CHECK(transport.emailBatches[1].size() == 1);
}

TEST_CASE("child Email notFound reconciles a changed Thread membership",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    RecordingTransport transport;
    transport.emailNotFound = "child-thread-1";
    transport.reconcileWithoutMissingChild = true;
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};

    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::app::ThreadMaterializationSummary>(result));
    CHECK(transport.batches.size() == 2);
    CHECK(transport.emailBatches.size() == 1);
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    const auto membership = threads.findMembership("account-1", "thread-1");
    const auto& current =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership);
    REQUIRE(current.has_value());
    CHECK(current->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Current);
    CHECK(current->thread.emailIds == std::vector<std::string>{"representative-thread-1"});
}

TEST_CASE("rapid child disappearance leaves Thread stale after bounded reconciliation",
          "[app][thread-materialization]")
{
    ensureApplication();
    Fixture fixture;
    fixture.seedRepresentative("thread-1");
    RecordingTransport transport;
    transport.emailNotFound = "child-thread-1";
    ConnectionProvider connections;
    javelin::app::ThreadMembershipMaterializationWorker worker{fixture.database, transport,
                                                               connections};

    const auto result = QCoro::waitFor(worker.materialize({
        .accountId = "account-1",
        .threadIds = {"thread-1"},
    }));

    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::Conflict);
    CHECK(transport.batches.size() == 3);
    CHECK(transport.emailBatches.size() == 3);
    javelin::jmap::cache::ThreadRepository threads{fixture.database};
    const auto membership = threads.findMembership("account-1", "thread-1");
    const auto& stale =
        std::get<std::optional<javelin::jmap::cache::ThreadMembershipRecord>>(membership);
    REQUIRE(stale.has_value());
    CHECK(stale->freshness == javelin::jmap::cache::ThreadMembershipFreshness::Stale);
}
