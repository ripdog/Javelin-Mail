#include "app/ThreadMembershipMaterializationWorker.h"
#include "app/AccountConnectionProvider.h"
#include "jmap/api/JmapMethodTransport.h"
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
#include <string>
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
                                              const std::vector<std::string>& notFound = {})
    {
        std::string list;
        for (const auto& id : ids)
        {
            if (!list.empty())
                list += ',';
            list += R"({"id":")" + id + R"(","emailIds":["representative-)" + id + R"(","child-)" +
                    id + R"("]})";
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

    class RecordingTransport final : public javelin::jmap::api::JmapMethodTransport
    {
      public:
        std::vector<std::vector<std::string>> batches;
        std::optional<std::string> notFound;
        bool omitLast = false;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::JmapMethodTransportResult>
        call(javelin::jmap::api::JmapMethodRequest request) override
        {
            REQUIRE(request.envelope.methodCalls.size() == 1);
            const auto& method = request.envelope.methodCalls.front();
            REQUIRE(method.name == "Thread/get");

            std::vector<std::string> ids;
            for (std::size_t number = 1; number <= 5; ++number)
            {
                const auto id = std::string{"thread-"} + std::to_string(number);
                if (method.arguments.contains('"' + id + '"'))
                    ids.push_back(id);
            }
            REQUIRE(ids.size() <= 2);
            batches.push_back(ids);
            std::vector<std::string> found;
            std::vector<std::string> missing;
            for (const auto& id : ids)
            {
                if (notFound == id)
                    missing.push_back(id);
                else
                    found.push_back(id);
            }
            if (omitLast && !found.empty())
                found.pop_back();
            co_return javelin::jmap::api::ResponseEnvelope{
                .methodResponses = {{
                    .name = "Thread/get",
                    .arguments = threadArguments(found, missing),
                    .callId = method.callId,
                }},
                .createdIds = std::nullopt,
                .sessionState = "session-state",
            };
        }
    };
} // namespace

TEST_CASE("Thread membership materialization batches exact ids and checkpoints child work",
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
    CHECK(summary.missingEmailIds == std::vector<std::string>{"child-thread-1", "child-thread-2",
                                                              "child-thread-3", "child-thread-4",
                                                              "child-thread-5"});
    REQUIRE(transport.batches.size() == 3);
    CHECK(transport.batches[0] == std::vector<std::string>{"thread-1", "thread-2"});
    CHECK(transport.batches[1] == std::vector<std::string>{"thread-3", "thread-4"});
    CHECK(transport.batches[2] == std::vector<std::string>{"thread-5"});
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
    transport.notFound = "thread-2";
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
