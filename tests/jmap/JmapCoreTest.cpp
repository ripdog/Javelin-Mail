#include "jmap/JmapCore.h"

#include "FixtureReader.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }

        static int argc = 1;
        static char appName[] = "javelin-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    class FakeTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::vector<javelin::jmap::api::TransportResult> queuedResults;
        std::vector<javelin::jmap::api::HttpRequest> requests;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest request) override
        {
            requests.push_back(std::move(request));
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
        return QStringLiteral("javelin-core-session-refresh-%1").arg(counter);
    }

    [[nodiscard]] std::string
    serializeResponseEnvelope(const javelin::jmap::api::ResponseEnvelope& envelope)
    {
        const auto serialized = javelin::jmap::api::serializeResponseEnvelope(envelope);
        REQUIRE(serialized.has_value());
        return *serialized;
    }

    [[nodiscard]] std::string emailFixtureWithIdentity(std::string_view emailId,
                                                       std::string_view threadId)
    {
        auto email = javelin::tests::loadFixture("jmap/entities/email.json");
        const auto idPosition = email.find(R"("id": "eml-1")");
        REQUIRE(idPosition != std::string::npos);
        email.replace(idPosition, std::string{R"("id": "eml-1")"}.size(),
                      R"("id": ")" + std::string{emailId} + '"');

        const auto threadPosition = email.find(R"("threadId": "thr-123")");
        REQUIRE(threadPosition != std::string::npos);
        email.replace(threadPosition, std::string{R"("threadId": "thr-123")"}.size(),
                      R"("threadId": ")" + std::string{threadId} + '"');
        return email;
    }
} // namespace

TEST_CASE("JmapCore startup session refresh discovers and caches websocket capability",
          "[jmap][core][session][websocket]")
{
    ensureApplication();
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
    });
    if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
    {
        FAIL(error->message.toStdString());
    }
    auto database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/websocket_session.json")),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{database, transport, methodTransport};

    const auto result = QCoro::waitFor(core.refreshSession(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "u1"));

    REQUIRE(std::holds_alternative<javelin::jmap::SessionRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::SessionRefreshSummary>(result);
    CHECK(summary.ownerAccountId == "u1");
    CHECK(summary.websocketAdvertised);
    CHECK(summary.websocketPushSupported);
    CHECK(summary.resolvedSessionUrl == "https://mail.example.com/.well-known/jmap");
    REQUIRE(transport.requests.size() == 1);
    CHECK(transport.requests.front().url ==
          QUrl{QStringLiteral("https://mail.example.com/.well-known/jmap")});

    javelin::jmap::cache::SessionRepository sessions{database};
    const auto loaded = sessions.load("u1");
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::api::Session>>(loaded));
    REQUIRE(std::get<std::optional<javelin::jmap::api::Session>>(loaded).has_value());
    const auto& session = *std::get<std::optional<javelin::jmap::api::Session>>(loaded);
    REQUIRE(session.capabilities.websocket.has_value());
    CHECK(session.capabilities.websocket->url == "wss://mail.example.com/jmap/ws");
    CHECK(session.capabilities.websocket->supportsPush);
}

TEST_CASE("JmapCore does not invent an initial mailbox when none is configured",
          "[jmap][core][settings]")
{
    ensureApplication();
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/websocket_session.json")),
    });
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(serializeResponseEnvelope({
            .methodResponses =
                {
                    {
                        .name = "Mailbox/get",
                        .arguments =
                            javelin::tests::loadFixture("jmap/method/mailbox_get_arguments.json"),
                        .callId = "mailboxes",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{database, transport, methodTransport};

    const auto result = QCoro::waitFor(core.refreshFromServer({
        .sessionUrl = "https://mail.example.com/.well-known/jmap",
        .loginEmail = "alice@example.com",
        .apiKey = "access-token",
    }));

    REQUIRE(std::holds_alternative<javelin::jmap::LiveRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
    CHECK_FALSE(summary.selectedMailboxId.has_value());
    CHECK(summary.emailCount == 0);
    CHECK(transport.requests.size() == 2);
}

TEST_CASE("JmapCore mailbox pages use one requested-page envelope", "[jmap][core][pagination]")
{
    ensureApplication();
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(
            javelin::tests::loadFixture("jmap/session/websocket_session.json")),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{database, transport, methodTransport};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://mail.example.com/.well-known/jmap",
        .loginEmail = "alice@example.com",
        .apiKey = "access-token",
    };
    REQUIRE(std::holds_alternative<javelin::jmap::SessionRefreshSummary>(
        QCoro::waitFor(core.refreshSession(settings, "u1"))));

    const auto email = javelin::tests::loadFixture("jmap/entities/email.json");
    const auto emailGetArguments =
        std::string{R"({"accountId":"u1","state":"email-state-1","list":[)"} + email +
        R"(],"notFound":[]})";
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(serializeResponseEnvelope({
            .methodResponses =
                {
                    {
                        .name = "Email/query",
                        .arguments =
                            R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":100,"ids":["eml-1"],"total":101,"limit":100})",
                        .callId = "page-query",
                    },
                    {
                        .name = "Email/get",
                        .arguments = emailGetArguments,
                        .callId = "page-representatives-get",
                    },
                    {
                        .name = "Thread/get",
                        .arguments =
                            R"({"accountId":"u1","state":"thread-state-1","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                        .callId = "page-threads-get",
                    },
                    {
                        .name = "Email/get",
                        .arguments = emailGetArguments,
                        .callId = "page-emails-get",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });

    const auto result = QCoro::waitFor(core.queryMailboxPage(settings, "u1", "mbx-inbox", 100));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxPageSummary>(result));
    const auto& page = std::get<javelin::jmap::MailboxPageSummary>(result);
    CHECK(page.offset == 100);
    CHECK(page.position == 100);
    CHECK(page.representativeCount == 1);
    CHECK(page.total == std::optional<std::size_t>{101});
    REQUIRE(page.results.size() == 1);
    CHECK(page.results.front().emailId == "eml-1");
    REQUIRE(transport.requests.size() == 2);
    CHECK(transport.requests.back().url ==
          QUrl{QStringLiteral("https://mail.example.com/jmap/api")});
}

TEST_CASE("JmapCore collapsed page baseline exposes thread fan-out beyond get limits",
          "[jmap][core][pagination][thread-materialization]")
{
    ensureApplication();
    QTemporaryDir temporaryDir;
    REQUIRE(temporaryDir.isValid());

    auto opened = javelin::jmap::cache::DatabaseConnection::open({
        .connectionName = makeConnectionName(),
        .databasePath = temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::cache::DatabaseConnection>(opened));
    auto database = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));

    auto tinySession = javelin::tests::loadFixture("jmap/session/websocket_session.json");
    const auto getLimitPosition = tinySession.find(R"("maxObjectsInGet": 500)");
    REQUIRE(getLimitPosition != std::string::npos);
    tinySession.replace(getLimitPosition, std::string{R"("maxObjectsInGet": 500)"}.size(),
                        R"("maxObjectsInGet": 2)");
    const auto setLimitPosition = tinySession.find(R"("maxObjectsInSet": 500)");
    REQUIRE(setLimitPosition != std::string::npos);
    tinySession.replace(setLimitPosition, std::string{R"("maxObjectsInSet": 500)"}.size(),
                        R"("maxObjectsInSet": 2)");

    FakeTransport transport;
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(tinySession),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::JmapCore core{database, transport, methodTransport};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://mail.example.com/.well-known/jmap",
        .loginEmail = "alice@example.com",
        .apiKey = "access-token",
    };
    REQUIRE(std::holds_alternative<javelin::jmap::SessionRefreshSummary>(
        QCoro::waitFor(core.refreshSession(settings, "u1"))));

    const auto firstRepresentative = emailFixtureWithIdentity("eml-1", "thr-1");
    const auto secondRepresentative = emailFixtureWithIdentity("eml-4", "thr-2");
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
                                        {
                                            .name = "Email/query",
                                            .arguments =
                                                R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":100,"ids":["eml-1","eml-4"],"total":102,"limit":100})",
                                            .callId = "page-query",
                                        },
                                        {
                                            .name = "Email/get",
                                            .arguments = std::string{R"({"accountId":"u1","state":"email-state-1","list":[)"} +
                                                         firstRepresentative +
                                                         ',' + secondRepresentative + R"(],"notFound":[]})",
                                            .callId = "page-representatives-get",
                                        },
                                        {
                                            .name = "Thread/get",
                                            .arguments =
                                                R"({"accountId":"u1","state":"thread-state-1","list":[{"id":"thr-1","emailIds":["eml-1","eml-2","eml-3"]},{"id":"thr-2","emailIds":["eml-4","eml-5"]}],"notFound":[]})",
                                            .callId = "page-threads-get",
                                        },
                                        {
                                            .name = "error",
                                            .arguments =
                                                R"({"type":"tooManyObjects","description":"The resolved Email/get contains 5 ids but maxObjectsInGet is 2."})",
                                            .callId = "page-emails-get",
                                        },
                                    },
                                .createdIds = std::nullopt,
                                .sessionState = "session-state-2",
                            })),
            });

    const auto result = QCoro::waitFor(core.queryMailboxPage(settings, "u1", "mbx-inbox", 100));
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).message.contains(
        QStringLiteral("maxObjectsInGet is 2")));
    REQUIRE(transport.requests.size() == 2);

    const auto requestEnvelope =
        javelin::jmap::api::parseRequestEnvelope(transport.requests.back().body.toStdString());
    REQUIRE(requestEnvelope.ok());
    REQUIRE(requestEnvelope.value.has_value());
    REQUIRE(requestEnvelope.value->methodCalls.size() == 4);
    CHECK(requestEnvelope.value->methodCalls[0].arguments.find(R"("limit":100)") !=
          std::string::npos);
    CHECK(requestEnvelope.value->methodCalls[3].name == "Email/get");
    CHECK(
        requestEnvelope.value->methodCalls[3].arguments.find(
            R"("#ids":{"resultOf":"page-threads-get","name":"Thread/get","path":"/list/*/emailIds"})") !=
        std::string::npos);
}
