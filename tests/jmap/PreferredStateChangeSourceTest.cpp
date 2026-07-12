#include "FixtureReader.h"
#include "jmap/api/SessionParser.h"
#include "jmap/cache/JmapTransportPreferenceRepository.h"
#include "jmap/cache/SessionRepository.h"
#include "jmap/sync/PreferredStateChangeSource.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <utility>
#include <variant>

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

    class FakeSource final : public javelin::jmap::sync::StateChangeSource
    {
      public:
        explicit FakeSource(javelin::jmap::sync::StateChangeSourceResult nextResult)
            : result(std::move(nextResult))
        {
        }

        std::size_t calls = 0;
        bool cancelled = false;
        javelin::jmap::sync::StateChangeSourceResult result;

        void cancel() override
        {
            cancelled = true;
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::sync::StateChangeSourceResult>
        consume(javelin::jmap::sync::StateChangeSubscription,
                javelin::jmap::sync::StateChangeConsumer&,
                javelin::jmap::sync::StateChangeCancellation&) override
        {
            ++calls;
            co_return result;
        }
    };

    class FakeConsumer final : public javelin::jmap::sync::StateChangeConsumer
    {
      public:
        [[nodiscard]] QCoro::Task<void>
        onStateChange(javelin::jmap::sync::StateChangeEvent) override
        {
            co_return;
        }
    };

    [[nodiscard]] QString makeConnectionName()
    {
        static int counter = 0;
        ++counter;
        return QStringLiteral("javelin-preferred-state-source-%1").arg(counter);
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
        context.connection =
            std::get<javelin::jmap::cache::DatabaseConnection>(std::move(opened));
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

    [[nodiscard]] javelin::jmap::sync::StateChangeSubscription
    subscription(std::string accountId = "u1")
    {
        return {
            .accountId = std::move(accountId),
            .lastState = "state-1",
            .types = {"Email", "Mailbox"},
        };
    }
} // namespace

TEST_CASE("preferred state changes honor remembered HTTP fallback",
          "[jmap][sync][websocket]")
{
    ensureApplication();
    auto database = makeDatabaseContext();
    const auto session = websocketSession();
    javelin::jmap::cache::SessionRepository sessions{database.connection};
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());

    javelin::jmap::cache::JmapTransportPreferenceRepository preferences{database.connection};
    REQUIRE_FALSE(preferences
                      .markHttpFallback("u1", session.capabilities.websocket->url,
                                        QDateTime::currentDateTimeUtc().addDays(1),
                                        QStringLiteral("previous failure"))
                      .has_value());

    auto websocket = std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    auto* websocketPointer = websocket.get();
    auto fallback = std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{
        .lastState = "state-2",
        .updateCount = 1,
    });
    auto* fallbackPointer = fallback.get();
    javelin::jmap::sync::PreferredStateChangeSource source{
        database.connection, "u1", session.capabilities.websocket->url, std::move(websocket),
        std::move(fallback)};
    FakeConsumer consumer;
    javelin::jmap::sync::StateChangeCancellation cancellation;

    const auto result = QCoro::waitFor(source.consume(subscription(), consumer, cancellation));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeStreamSummary>(result));
    CHECK(websocketPointer->calls == 0);
    CHECK(fallbackPointer->calls == 1);
}

TEST_CASE("preferred state changes persist owner fallback after websocket failure",
          "[jmap][sync][websocket]")
{
    ensureApplication();
    auto database = makeDatabaseContext();
    auto session = websocketSession();
    session.accounts.emplace("u2", session.accounts.at("u1"));
    session.accounts.at("u2").name = "Shared";
    javelin::jmap::cache::SessionRepository sessions{database.connection};
    REQUIRE_FALSE(sessions.replace("u1", session).has_value());

    auto websocket = std::make_unique<FakeSource>(javelin::jmap::api::TransportError{
        .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
        .message = "handshake failed",
        .httpStatus = std::nullopt,
    });
    auto* websocketPointer = websocket.get();
    auto fallback = std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{
        .lastState = "state-2",
        .updateCount = 1,
    });
    auto* fallbackPointer = fallback.get();
    javelin::jmap::sync::PreferredStateChangeSource source{
        database.connection, "u2", session.capabilities.websocket->url, std::move(websocket),
        std::move(fallback)};
    FakeConsumer consumer;
    javelin::jmap::sync::StateChangeCancellation cancellation;

    const auto result =
        QCoro::waitFor(source.consume(subscription("u2"), consumer, cancellation));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeStreamSummary>(result));
    CHECK(websocketPointer->calls == 1);
    CHECK(fallbackPointer->calls == 1);

    javelin::jmap::cache::JmapTransportPreferenceRepository preferences{database.connection};
    const auto resolved = preferences.resolve("u2");
    REQUIRE(std::holds_alternative<
            std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved));
    const auto& target =
        *std::get<std::optional<javelin::jmap::cache::JmapTransportTarget>>(resolved);
    CHECK(target.ownerAccountId == "u1");
    CHECK(target.mode == javelin::jmap::cache::JmapTransportMode::HttpFallback);
    REQUIRE(target.lastError.has_value());
    CHECK(*target.lastError == QStringLiteral("handshake failed"));
}
