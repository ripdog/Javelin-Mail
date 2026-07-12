#include "FixtureReader.h"
#include "jmap/api/SessionParser.h"
#include "jmap/cache/JmapTransportPreferenceRepository.h"
#include "jmap/cache/SessionRepository.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

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
        return QStringLiteral("javelin-transport-preference-%1").arg(counter);
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
        context.connection =
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(result));
        return context;
    }

    [[nodiscard]] javelin::jmap::api::Session websocketSession()
    {
        const auto parsed = javelin::jmap::api::parseSession(
            javelin::tests::loadFixture("jmap/session/basic_session.json"),
            {.mail = true, .submission = true});
        REQUIRE(parsed.ok());
        REQUIRE(parsed.session.has_value());
        auto session = *parsed.session;
        session.capabilities.websocket = javelin::jmap::api::WebSocketCapability{
            .url = "wss://mail.example.com/jmap/ws",
            .supportsPush = true,
        };
        return session;
    }
} // namespace

TEST_CASE("transport preferences remember HTTP fallback for an advertised websocket",
          "[jmap][cache][transport]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    auto session = websocketSession();
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());

    javelin::jmap::cache::JmapTransportPreferenceRepository preferences{
        databaseContext.connection};
    auto resolved = preferences.resolve("u1");
    REQUIRE(std::holds_alternative<
            std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved));
    REQUIRE(std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved)
                .has_value());
    auto target = *std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved);
    CHECK(target.mode == javelin::jmap::cache::JmapTransportMode::Unknown);
    CHECK(target.shouldAttemptWebSocket(QDateTime::currentDateTimeUtc()));

    const auto now = QDateTime::currentDateTimeUtc();
    const auto retryAfter = now.addSecs(3600);
    REQUIRE_FALSE(preferences
                      .markHttpFallback("u1", target.webSocketUrl, retryAfter,
                                        QStringLiteral("handshake failed"))
                      .has_value());

    resolved = preferences.resolve("u1");
    REQUIRE(std::holds_alternative<
            std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved));
    target = *std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved);
    CHECK(target.mode == javelin::jmap::cache::JmapTransportMode::HttpFallback);
    REQUIRE(target.retryAfter.has_value());
    CHECK_FALSE(target.shouldAttemptWebSocket(now));
    CHECK(target.shouldAttemptWebSocket(retryAfter.addMSecs(1)));
    REQUIRE(target.lastError.has_value());
    CHECK(*target.lastError == QStringLiteral("handshake failed"));

    REQUIRE_FALSE(sessions.replace("u1", session).has_value());
    resolved = preferences.resolve("u1");
    target = *std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved);
    CHECK(target.mode == javelin::jmap::cache::JmapTransportMode::HttpFallback);

    session.capabilities.websocket->url = "wss://mail.example.com/jmap/ws-v2";
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());
    resolved = preferences.resolve("u1");
    target = *std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved);
    CHECK(target.mode == javelin::jmap::cache::JmapTransportMode::Unknown);
    CHECK(target.webSocketUrl == "wss://mail.example.com/jmap/ws-v2");
}

TEST_CASE("transport preferences record a working websocket", "[jmap][cache][transport]")
{
    ApplicationGuard application;
    Q_UNUSED(application);

    auto databaseContext = makeDatabaseContext();
    javelin::jmap::cache::SessionRepository sessions{databaseContext.connection};
    const auto session = websocketSession();
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());

    javelin::jmap::cache::JmapTransportPreferenceRepository preferences{
        databaseContext.connection};
    REQUIRE_FALSE(
        preferences.markWebSocketAvailable("u1", session.capabilities.websocket->url).has_value());

    const auto resolved = preferences.resolve("u1");
    REQUIRE(std::holds_alternative<
            std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved));
    const auto& target =
        *std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved);
    CHECK(target.mode == javelin::jmap::cache::JmapTransportMode::WebSocket);
    CHECK(target.shouldAttemptWebSocket(QDateTime::currentDateTimeUtc()));
    CHECK_FALSE(target.retryAfter.has_value());
    CHECK_FALSE(target.lastError.has_value());
}
