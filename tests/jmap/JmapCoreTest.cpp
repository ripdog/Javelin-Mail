#include "jmap/AccountBootstrapClient.h"
#include "jmap/api/SessionRefreshClient.h"
#include "jmap/query/MailQueryClient.h"
#include "jmap/query/MailQueryMaterializer.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include "FixtureReader.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/MailboxWindowRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/cache/SyncStateRepository.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QSqlQuery>
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

    [[nodiscard]] std::string emailGetArguments(std::string_view accountId, std::string_view state,
                                                std::string_view emailJson)
    {
        return std::string{R"({"accountId":")"} + std::string{accountId} + R"(","state":")" +
               std::string{state} + R"(","list":[)" + std::string{emailJson} +
               R"(],"notFound":[]})";
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

    [[nodiscard]] std::string unreadEmailFixture()
    {
        auto email = emailFixtureWithIdentity("eml-1", "thr-123");
        const auto seenPosition = email.find(R"("$seen": true)");
        REQUIRE(seenPosition != std::string::npos);
        email.replace(seenPosition, std::string{R"("$seen": true)"}.size(), R"("$seen": false)");
        return email;
    }
} // namespace

TEST_CASE("SessionRefreshClient discovers and caches websocket capability",
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
    javelin::jmap::SessionRefreshClient sessionRefresh{database, transport};

    const auto result = QCoro::waitFor(sessionRefresh.refresh(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "connection-1", "u1", "u1"));

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

TEST_CASE("AccountBootstrapClient does not invent an initial mailbox when none is configured",
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
                    {
                        .name = "Email/get",
                        .arguments =
                            R"({"accountId":"u1","state":"email-state-1","list":[],"notFound":[]})",
                        .callId = "initial-email-state",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });
    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::AccountBootstrapClient bootstrap{database, transport, methodTransport};

    const auto result = QCoro::waitFor(bootstrap.bootstrap(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "connection-1"));

    REQUIRE(std::holds_alternative<javelin::jmap::LiveRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
    CHECK_FALSE(summary.selectedMailboxId.has_value());
    CHECK(summary.emailCount == 0);
    REQUIRE(transport.requests.size() == 2);
    CHECK(transport.requests.back().body.contains("\"Mailbox/get\""));
    CHECK(transport.requests.back().body.contains("\"Email/get\""));
    CHECK(transport.requests.back().body.contains("\"ids\":[]"));
    CHECK_FALSE(transport.requests.back().body.contains("\"Email/query\""));

    javelin::jmap::cache::SyncStateRepository syncStates{database};
    const auto emailState =
        syncStates.find({.accountId = summary.accountId, .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState)->stateToken ==
          "email-state-1");

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
            .sessionState = "session-state-3",
        })),
    });

    const auto repeatedResult = QCoro::waitFor(bootstrap.bootstrap(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "connection-1"));
    REQUIRE(std::holds_alternative<javelin::jmap::LiveRefreshSummary>(repeatedResult));
    REQUIRE(transport.requests.size() == 4);
    CHECK(transport.requests.back().body.contains("\"Mailbox/get\""));
    CHECK_FALSE(transport.requests.back().body.contains("\"Email/get\""));

    const auto repeatedEmailState =
        syncStates.find({.accountId = summary.accountId, .objectType = "Email", .queryKey = {}});
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(
        repeatedEmailState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(repeatedEmailState)
                .has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(repeatedEmailState)
              ->stateToken == "email-state-1");
}

TEST_CASE("AccountBootstrapClient baselines historical unread mail before configured window "
          "materialization",
          "[jmap][core][bootstrap][notification]")
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

    const auto unreadEmail = unreadEmailFixture();
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
                    {
                        .name = "Email/get",
                        .arguments =
                            R"({"accountId":"u1","state":"email-state-1","list":[],"notFound":[]})",
                        .callId = "initial-email-state",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });
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
                                                R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":0,"ids":["eml-1"],"total":1})",
                                            .callId = "mailbox-query",
                                        },
                                        {
                                            .name = "Email/get",
                                            .arguments = emailGetArguments("u1", "email-state-1",
                                                                           unreadEmail),
                                            .callId = "thread-ids-get",
                                        },
                                        {
                                            .name = "Thread/get",
                                            .arguments =
                                                R"({"accountId":"u1","state":"thread-state-1","list":[{"id":"thr-123","emailIds":["eml-1"]}],"notFound":[]})",
                                            .callId = "threads-get",
                                        },
                                        {
                                            .name = "Email/get",
                                            .arguments =
                                                emailGetArguments("u1",
                                                                  "email-state-1", unreadEmail),
                                            .callId = "mailbox-emails-get",
                                        },
                                    },
                                .createdIds = std::nullopt,
                                .sessionState = "session-state-3",
                            })),
            });

    javelin::jmap::api::HttpJmapMethodTransport methodTransport{transport};
    javelin::jmap::AccountBootstrapClient bootstrap{database, transport, methodTransport};
    const auto result = QCoro::waitFor(bootstrap.bootstrap(
        {
            .sessionUrl = "https://mail.example.com/.well-known/jmap",
            .loginEmail = "alice@example.com",
            .apiKey = "access-token",
        },
        "connection-1", {}, {"mbx-inbox"}));

    REQUIRE(std::holds_alternative<javelin::jmap::LiveRefreshSummary>(result));
    const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
    CHECK(summary.accountId == "u1");
    CHECK(summary.selectedMailboxId == std::optional<std::string>{"mbx-inbox"});
    CHECK(summary.emailCount == 1);
    REQUIRE(transport.requests.size() == 3);
    CHECK(transport.requests[1].body.contains("\"initial-email-state\""));
    CHECK(transport.requests[1].body.contains("\"ids\":[]"));
    CHECK_FALSE(transport.requests[1].body.contains("\"Email/query\""));
    CHECK(transport.requests[2].body.contains("\"Email/query\""));
    CHECK(transport.requests[2].body.contains("mbx-inbox"));

    javelin::jmap::cache::SyncStateRepository syncStates{database};
    const auto emailState =
        syncStates.find({.accountId = "u1", .objectType = "Email", .queryKey = {}});
    REQUIRE(
        std::holds_alternative<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState).has_value());
    CHECK(std::get<std::optional<javelin::jmap::cache::SyncStateRecord>>(emailState)->stateToken ==
          "email-state-1");

    const auto inboxQueryKey = javelin::jmap::sync::mailboxQueryKey({
        .mailboxId = "mbx-inbox",
        .sortProperty = "receivedAt",
        .isAscending = false,
        .collapseThreads = true,
    });
    javelin::jmap::cache::MailboxWindowRepository windows{database};
    const auto windowResult = windows.find("u1", inboxQueryKey, 0, 100);
    REQUIRE(std::holds_alternative<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(
        windowResult));
    const auto& window =
        std::get<std::optional<javelin::jmap::cache::MailboxWindowRecord>>(windowResult);
    REQUIRE(window.has_value());
    CHECK(window->queryState == "query-state-1");
    CHECK(window->emailIds == std::vector<std::string>{"eml-1"});

    QSqlQuery notifications{database.database()};
    REQUIRE(notifications.exec(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM mail_notification_state WHERE account_id='u1'),"
        "(SELECT COUNT(*) FROM mail_notification_event_outbox WHERE account_id='u1')")));
    REQUIRE(notifications.next());
    CHECK(notifications.value(0).toInt() == 0);
    CHECK(notifications.value(1).toInt() == 0);
}

TEST_CASE("AccountBootstrapClient isolates identical remote account ids across connections",
          "[jmap][core][account-identity]")
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

    const auto queueBootstrapResponses = [](FakeTransport& transport)
    {
        transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
            .statusCode = 200,
            .body = QByteArray::fromStdString(
                javelin::tests::loadFixture("jmap/session/websocket_session.json")),
        });
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
                                                .name = "Mailbox/get",
                                                .arguments = javelin::tests::loadFixture(
                                                    "jmap/method/mailbox_get_arguments.json"),
                                                .callId = "mailboxes",
                                            },
                                            {
                                                .name = "Email/get",
                                                .arguments =
                                                    R"({"accountId":"u1","state":"email-state-1","list":[],"notFound":[]})",
                                                .callId = "initial-email-state",
                                            },
                                        },
                                    .createdIds = std::nullopt,
                                    .sessionState = "session-state-2",
                                })),
                });
    };

    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://mail.example.com/.well-known/jmap",
        .loginEmail = "alice@example.com",
        .apiKey = "access-token",
    };

    FakeTransport firstTransport;
    queueBootstrapResponses(firstTransport);
    javelin::jmap::api::HttpJmapMethodTransport firstMethodTransport{firstTransport};
    javelin::jmap::AccountBootstrapClient firstBootstrap{database, firstTransport,
                                                         firstMethodTransport};
    const auto firstResult = QCoro::waitFor(firstBootstrap.bootstrap(settings, "connection-a"));
    REQUIRE(std::holds_alternative<javelin::jmap::LiveRefreshSummary>(firstResult));
    const auto& firstSummary = std::get<javelin::jmap::LiveRefreshSummary>(firstResult);
    CHECK(firstSummary.accountId == "u1");

    FakeTransport secondTransport;
    queueBootstrapResponses(secondTransport);
    javelin::jmap::api::HttpJmapMethodTransport secondMethodTransport{secondTransport};
    javelin::jmap::AccountBootstrapClient secondBootstrap{database, secondTransport,
                                                          secondMethodTransport};
    const auto secondResult = QCoro::waitFor(secondBootstrap.bootstrap(settings, "connection-b"));
    REQUIRE(std::holds_alternative<javelin::jmap::LiveRefreshSummary>(secondResult));
    const auto& secondSummary = std::get<javelin::jmap::LiveRefreshSummary>(secondResult);
    CHECK(secondSummary.accountId != "u1");
    CHECK(secondSummary.accountId != firstSummary.accountId);

    REQUIRE(secondTransport.requests.size() == 2);
    CHECK(secondTransport.requests.back().body.contains("\"Mailbox/get\""));
    CHECK(secondTransport.requests.back().body.contains("\"accountId\":\"u1\""));
    CHECK_FALSE(secondTransport.requests.back().body.contains(
        QByteArray::fromStdString("\"accountId\":\"" + secondSummary.accountId + "\"")));
    REQUIRE(secondTransport.requests.front().authentication.has_value());
    CHECK(secondTransport.requests.front().authentication->accountId == "connection-b");

    QSqlQuery accounts{database.database()};
    REQUIRE(accounts.exec(
        QStringLiteral("SELECT account_id,connection_id,remote_account_id FROM accounts WHERE "
                       "remote_account_id='u1' ORDER BY connection_id")));
    REQUIRE(accounts.next());
    CHECK(accounts.value(0).toString() == QStringLiteral("u1"));
    CHECK(accounts.value(1).toString() == QStringLiteral("connection-a"));
    CHECK(accounts.value(2).toString() == QStringLiteral("u1"));
    REQUIRE(accounts.next());
    CHECK(accounts.value(0).toString() == QString::fromStdString(secondSummary.accountId));
    CHECK(accounts.value(1).toString() == QStringLiteral("connection-b"));
    CHECK(accounts.value(2).toString() == QStringLiteral("u1"));
    CHECK_FALSE(accounts.next());

    QSqlQuery secondMailboxes{database.database()};
    secondMailboxes.prepare(
        QStringLiteral("SELECT COUNT(*) FROM mailboxes WHERE account_id=:account_id"));
    secondMailboxes.bindValue(QStringLiteral(":account_id"),
                              QString::fromStdString(secondSummary.accountId));
    REQUIRE(secondMailboxes.exec());
    REQUIRE(secondMailboxes.next());
    CHECK(secondMailboxes.value(0).toInt() > 0);
}

TEST_CASE("MailQueryMaterializer mailbox pages use one requested-page envelope",
          "[jmap][query][pagination]")
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
    javelin::jmap::SessionRefreshClient sessionRefresh{database, transport};
    javelin::jmap::MailQueryClient queryClient{database, methodTransport};
    javelin::jmap::MailQueryMaterializer queryMaterializer{database, queryClient};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://mail.example.com/.well-known/jmap",
        .loginEmail = "alice@example.com",
        .apiKey = "access-token",
    };
    REQUIRE(std::holds_alternative<javelin::jmap::SessionRefreshSummary>(
        QCoro::waitFor(sessionRefresh.refresh(settings, "connection-1", "u1", "u1"))));

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

    const auto result =
        QCoro::waitFor(queryMaterializer.queryMailboxPage(settings, "u1", "mbx-inbox", 100));
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

TEST_CASE("MailQueryMaterializer collapsed page materializes representatives within get limits",
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
    javelin::jmap::SessionRefreshClient sessionRefresh{database, transport};
    javelin::jmap::MailQueryClient queryClient{database, methodTransport};
    javelin::jmap::MailQueryMaterializer queryMaterializer{database, queryClient};
    const javelin::jmap::LiveConnectionSettings settings{
        .sessionUrl = "https://mail.example.com/.well-known/jmap",
        .loginEmail = "alice@example.com",
        .apiKey = "access-token",
    };
    REQUIRE(std::holds_alternative<javelin::jmap::SessionRefreshSummary>(
        QCoro::waitFor(sessionRefresh.refresh(settings, "connection-1", "u1", "u1"))));

    const auto firstRepresentative = emailFixtureWithIdentity("eml-1", "thr-1");
    const auto secondRepresentative = emailFixtureWithIdentity("eml-4", "thr-2");
    transport.queuedResults.push_back(javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(serializeResponseEnvelope({
            .methodResponses =
                {
                    {
                        .name = "Email/query",
                        .arguments =
                            R"({"accountId":"u1","queryState":"query-state-1","canCalculateChanges":true,"position":100,"ids":["eml-1","eml-4"],"total":102,"limit":2})",
                        .callId = "page-query",
                    },
                    {
                        .name = "Email/get",
                        .arguments =
                            std::string{R"({"accountId":"u1","state":"email-state-1","list":[)"} +
                            firstRepresentative + ',' + secondRepresentative +
                            R"(],"notFound":[]})",
                        .callId = "page-representatives-get",
                    },
                },
            .createdIds = std::nullopt,
            .sessionState = "session-state-2",
        })),
    });

    const auto result =
        QCoro::waitFor(queryMaterializer.queryMailboxPage(settings, "u1", "mbx-inbox", 100));
    REQUIRE(std::holds_alternative<javelin::jmap::MailboxPageSummary>(result));
    const auto& page = std::get<javelin::jmap::MailboxPageSummary>(result);
    CHECK(page.position == 100);
    CHECK(page.returnedLimit == 2);
    CHECK(page.representativeCount == 2);
    CHECK(page.total == std::optional<std::size_t>{102});
    REQUIRE(page.results.size() == 2);
    CHECK(page.results[0].emailId == "eml-1");
    CHECK(page.results[1].emailId == "eml-4");
    CHECK_FALSE(page.results[0].globalThreadMessageCount.has_value());
    REQUIRE(transport.requests.size() == 2);

    const auto requestEnvelope =
        javelin::jmap::api::parseRequestEnvelope(transport.requests.back().body.toStdString());
    REQUIRE(requestEnvelope.ok());
    REQUIRE(requestEnvelope.value.has_value());
    REQUIRE(requestEnvelope.value->methodCalls.size() == 2);
    CHECK(requestEnvelope.value->methodCalls[0].arguments.find(R"("limit":2)") !=
          std::string::npos);
    CHECK(requestEnvelope.value->methodCalls[1].name == "Email/get");
    CHECK(requestEnvelope.value->methodCalls[1].arguments.find(
              R"("#ids":{"resultOf":"page-query","name":"Email/query","path":"/ids"})") !=
          std::string::npos);
}
