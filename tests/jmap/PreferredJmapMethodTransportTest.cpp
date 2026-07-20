#include "FixtureReader.h"
#include "jmap/api/JmapMethodTransport.h"
#include "jmap/api/MethodEnvelope.h"
#include "jmap/api/SessionParser.h"
#include "jmap/api/Transport.h"
#include "jmap/cache/SessionRepository.h"

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

    class FakeHttpTransport final : public javelin::jmap::api::AbstractTransport
    {
      public:
        std::size_t calls = 0;
        javelin::jmap::api::TransportResult result;

        [[nodiscard]] QCoro::Task<javelin::jmap::api::TransportResult>
        send(javelin::jmap::api::HttpRequest) override
        {
            ++calls;
            co_return result;
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-preferred-method-transport-%1").arg(counter);
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
        auto opened = javelin::jmap::cache::DatabaseConnection::open({
            .connectionName = makeConnectionName(),
            .databasePath = context.temporaryDir.filePath(QStringLiteral("cache.sqlite3")),
        });
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&opened))
        {
            FAIL(error->message.toStdString());
        }
        context.connection = std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
        return context;
    }

    [[nodiscard]] javelin::jmap::api::Session sessionWithWebSocket(std::string url)
    {
        const auto parsed = javelin::jmap::api::parseSession(
            javelin::tests::loadFixture("jmap/session/basic_session.json"),
            {.mail = true, .submission = true});
        REQUIRE(parsed.ok());
        REQUIRE(parsed.session.has_value());
        auto session = *parsed.session;
        session.capabilities.websocket = javelin::jmap::api::WebSocketCapability{
            .url = std::move(url),
            .supportsPush = true,
        };
        return session;
    }

    [[nodiscard]] javelin::jmap::api::RequestEnvelope requestEnvelope()
    {
        const auto parsed = javelin::jmap::api::parseRequestEnvelope(
            javelin::tests::loadFixture("jmap/method/request.json"));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.value.has_value());
        return *parsed.value;
    }

    [[nodiscard]] javelin::jmap::api::JmapMethodRequest methodRequest()
    {
        return javelin::jmap::api::JmapMethodRequest{
            .accountId = "u1",
            .apiUrl = "https://mail.example.com/jmap/api",
            .accessToken = "access-token",
            .envelope = requestEnvelope(),
            .cancellation = {},
        };
    }
} // namespace

TEST_CASE("preferred JMAP transport falls back to HTTP before websocket dispatch",
          "[jmap][method][transport][websocket]")
{
    ensureApplication();
    auto database = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{database.connection};
    const auto session = sessionWithWebSocket("https://mail.example.com/not-a-websocket");
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());

    FakeHttpTransport resourceTransport;
    resourceTransport.result = javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(javelin::tests::loadFixture("jmap/method/response.json")),
    };
    javelin::jmap::api::HttpJmapMethodTransport httpTransport{resourceTransport};
    javelin::jmap::api::WebSocketFailureCooldowns cooldowns;
    javelin::jmap::api::PreferredJmapMethodTransport preferred{database.connection, httpTransport,
                                                               cooldowns};

    const auto result = QCoro::waitFor(preferred.call(methodRequest()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(result));
    CHECK(resourceTransport.calls == 1);

    CHECK(cooldowns.retryDelay(session.capabilities.websocket->url).has_value());
}

TEST_CASE("preferred JMAP transport ignores remembered HTTP fallback after restart",
          "[jmap][method][transport][websocket]")
{
    ensureApplication();
    auto database = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{database.connection};
    const auto session = sessionWithWebSocket("wss://127.0.0.1:1/jmap");
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());

    QSqlQuery staleFallback{database.connection.database()};
    staleFallback.prepare(QStringLiteral(
        "INSERT INTO jmap_transport_preferences(owner_account_id,websocket_url,mode,retry_after,"
        "last_error) VALUES('u1',:url,'http_fallback','2999-01-01T00:00:00.000Z','old failure')"));
    staleFallback.bindValue(QStringLiteral(":url"),
                            QString::fromStdString(session.capabilities.websocket->url));
    REQUIRE(staleFallback.exec());

    FakeHttpTransport resourceTransport;
    resourceTransport.result = javelin::jmap::api::HttpResponse{
        .statusCode = 200,
        .body = QByteArray::fromStdString(javelin::tests::loadFixture("jmap/method/response.json")),
    };
    javelin::jmap::api::HttpJmapMethodTransport httpTransport{resourceTransport};
    javelin::jmap::api::WebSocketFailureCooldowns cooldowns;
    javelin::jmap::api::PreferredJmapMethodTransport preferred{database.connection, httpTransport,
                                                               cooldowns};

    const auto result = QCoro::waitFor(preferred.call(methodRequest()));

    REQUIRE(std::holds_alternative<javelin::jmap::api::ResponseEnvelope>(result));
    CHECK(resourceTransport.calls == 1);
    CHECK(cooldowns.retryDelay(session.capabilities.websocket->url).has_value());
}
