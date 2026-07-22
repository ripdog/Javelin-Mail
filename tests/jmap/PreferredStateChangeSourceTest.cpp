#include "jmap/sync/PreferredStateChangeSource.h"
#include "FixtureReader.h"
#include "jmap/api/SessionParser.h"

#include <QCoroTask>
#include <QCoroTimer>

#include <QCoreApplication>
#include <QTimer>

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

    class CancellableWaitingSource final : public javelin::jmap::sync::StateChangeSource
    {
      public:
        std::size_t calls = 0;

        void cancel() override
        {
            if (m_timer != nullptr)
                m_timer->start(std::chrono::milliseconds{0});
        }

        [[nodiscard]] QCoro::Task<javelin::jmap::sync::StateChangeSourceResult>
        consume(javelin::jmap::sync::StateChangeSubscription,
                javelin::jmap::sync::StateChangeConsumer&,
                javelin::jmap::sync::StateChangeCancellation&) override
        {
            ++calls;
            QTimer timer;
            timer.setSingleShot(true);
            timer.start(std::chrono::hours{1});
            m_timer = &timer;
            co_await qCoro(timer).waitForTimeout();
            m_timer = nullptr;
            co_return javelin::jmap::api::TransportError{
                .code = javelin::jmap::api::TransportErrorCode::Cancelled,
                .message = "cancelled",
            };
        }

      private:
        QTimer* m_timer = nullptr;
    };

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

TEST_CASE("a new preferred state-change source attempts websocket", "[jmap][sync][websocket]")
{
    ensureApplication();
    const auto session = websocketSession();

    auto websocket = std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    auto* websocketPointer = websocket.get();
    auto fallback = std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{
        .lastState = "state-2",
        .updateCount = 1,
    });
    auto* fallbackPointer = fallback.get();
    javelin::jmap::api::WebSocketFailureCooldowns cooldowns;
    javelin::jmap::sync::PreferredStateChangeSource source{
        cooldowns, session.capabilities.websocket->url, std::move(websocket), std::move(fallback)};
    FakeConsumer consumer;
    javelin::jmap::sync::StateChangeCancellation cancellation;

    const auto result = QCoro::waitFor(source.consume(subscription(), consumer, cancellation));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeStreamSummary>(result));
    CHECK(websocketPointer->calls == 1);
    CHECK(fallbackPointer->calls == 0);
}

TEST_CASE("preferred state changes remember websocket failure only in the current process",
          "[jmap][sync][websocket]")
{
    ensureApplication();
    const auto session = websocketSession();
    javelin::jmap::api::WebSocketFailureCooldowns cooldowns;

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
        cooldowns, session.capabilities.websocket->url, std::move(websocket), std::move(fallback)};
    FakeConsumer consumer;
    javelin::jmap::sync::StateChangeCancellation cancellation;

    const auto result = QCoro::waitFor(source.consume(subscription(), consumer, cancellation));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeStreamSummary>(result));
    CHECK(websocketPointer->calls == 1);
    CHECK(fallbackPointer->calls == 1);
    const auto retryDelay = cooldowns.retryDelay(session.capabilities.websocket->url);
    REQUIRE(retryDelay.has_value());
    CHECK(*retryDelay >= std::chrono::minutes{14});
    CHECK(*retryDelay <= std::chrono::minutes{15});

    auto suppressedWebSocket =
        std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    auto* suppressedWebSocketPointer = suppressedWebSocket.get();
    auto suppressedFallback =
        std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    auto* suppressedFallbackPointer = suppressedFallback.get();
    javelin::jmap::sync::PreferredStateChangeSource suppressed{
        cooldowns, session.capabilities.websocket->url, std::move(suppressedWebSocket),
        std::move(suppressedFallback)};
    static_cast<void>(QCoro::waitFor(suppressed.consume(subscription(), consumer, cancellation)));
    CHECK(suppressedWebSocketPointer->calls == 0);
    CHECK(suppressedFallbackPointer->calls == 1);

    javelin::jmap::api::WebSocketFailureCooldowns nextProcessCooldowns;
    auto nextProcessWebSocket =
        std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    auto* nextProcessWebSocketPointer = nextProcessWebSocket.get();
    auto nextProcessFallback =
        std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    javelin::jmap::sync::PreferredStateChangeSource nextProcess{
        nextProcessCooldowns, session.capabilities.websocket->url, std::move(nextProcessWebSocket),
        std::move(nextProcessFallback)};
    static_cast<void>(QCoro::waitFor(nextProcess.consume(subscription(), consumer, cancellation)));
    CHECK(nextProcessWebSocketPointer->calls == 1);
}

TEST_CASE("preferred state changes retry websocket when the fallback cooldown expires",
          "[jmap][sync][websocket]")
{
    ensureApplication();
    const auto session = websocketSession();
    javelin::jmap::api::WebSocketFailureCooldowns cooldowns{std::chrono::milliseconds{20}};
    cooldowns.recordFailure(session.capabilities.websocket->url);

    auto websocket = std::make_unique<FakeSource>(javelin::jmap::sync::StateChangeStreamSummary{});
    auto* websocketPointer = websocket.get();
    auto fallback = std::make_unique<CancellableWaitingSource>();
    auto* fallbackPointer = fallback.get();
    javelin::jmap::sync::PreferredStateChangeSource source{
        cooldowns, session.capabilities.websocket->url, std::move(websocket), std::move(fallback)};
    FakeConsumer consumer;
    javelin::jmap::sync::StateChangeCancellation cancellation;

    const auto result = QCoro::waitFor(source.consume(subscription(), consumer, cancellation));

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeStreamSummary>(result));
    CHECK(fallbackPointer->calls == 1);
    CHECK(websocketPointer->calls == 1);
}
