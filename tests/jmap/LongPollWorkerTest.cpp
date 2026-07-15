#include "jmap/sync/LongPollWorker.h"

#include <QCoroTask>

#include <QCoreApplication>

#include <catch2/catch_test_macros.hpp>

#include <memory>
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

    class FakeStateChangeSource final : public javelin::jmap::sync::StateChangeSource
    {
      public:
        std::vector<javelin::jmap::sync::StateChangeSubscription> subscriptions;
        std::vector<
            std::variant<javelin::jmap::sync::StateChangeEvent, javelin::jmap::api::TransportError>>
            queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::sync::StateChangeSourceResult>
        consume(javelin::jmap::sync::StateChangeSubscription subscription,
                javelin::jmap::sync::StateChangeConsumer& consumer,
                javelin::jmap::sync::StateChangeCancellation& cancellation) override
        {
            subscriptions.push_back(subscription);
            REQUIRE_FALSE(queuedResults.empty());

            javelin::jmap::sync::StateChangeStreamSummary summary{
                .lastState = subscription.lastState,
                .updateCount = 0,
            };
            while (!queuedResults.empty() && !cancellation.isCancelled())
            {
                auto result = std::move(queuedResults.front());
                queuedResults.erase(queuedResults.begin());
                if (const auto* error = std::get_if<javelin::jmap::api::TransportError>(&result))
                {
                    co_return *error;
                }

                auto event = std::get<javelin::jmap::sync::StateChangeEvent>(std::move(result));
                summary.lastState = event.newState;
                if (event.notifyConsumer)
                {
                    ++summary.updateCount;
                    co_await consumer.onStateChange(std::move(event));
                }
            }

            co_return summary;
        }
    };

    class FakeStateChangeConsumer final : public javelin::jmap::sync::StateChangeConsumer
    {
      public:
        std::vector<javelin::jmap::sync::StateChangeEvent> updates;
        javelin::jmap::sync::StateChangeCancellation* cancellation = nullptr;
        std::optional<std::size_t> cancelAfterUpdates;

        [[nodiscard]] QCoro::Task<void>
        onStateChange(javelin::jmap::sync::StateChangeEvent event) override
        {
            updates.push_back(event);
            if (cancellation != nullptr && cancelAfterUpdates.has_value() &&
                updates.size() >= *cancelAfterUpdates)
            {
                cancellation->cancel();
            }
            co_return;
        }
    };

    class FakeStateChangeSleeper final : public javelin::jmap::sync::StateChangeSleeper
    {
      public:
        std::vector<std::chrono::milliseconds> delays;

        [[nodiscard]] QCoro::Task<void> sleepFor(const std::chrono::milliseconds delay) override
        {
            delays.push_back(delay);
            co_return;
        }
    };

    [[nodiscard]] javelin::jmap::sync::StateChangeSubscription makeSubscription()
    {
        return javelin::jmap::sync::StateChangeSubscription{
            .accountId = "account-1",
            .lastState = "state-1",
            .types = {"Email", "Mailbox"},
        };
    }

} // namespace

TEST_CASE("backoff policy doubles delays up to the configured maximum", "[jmap][sync]")
{
    const javelin::jmap::sync::BackoffPolicy policy{
        .initialDelay = std::chrono::milliseconds{1000},
        .maxDelay = std::chrono::milliseconds{8000},
    };

    CHECK(policy.delayForAttempt(0) == std::chrono::milliseconds{0});
    CHECK(policy.delayForAttempt(1) == std::chrono::milliseconds{1000});
    CHECK(policy.delayForAttempt(2) == std::chrono::milliseconds{2000});
    CHECK(policy.delayForAttempt(3) == std::chrono::milliseconds{4000});
    CHECK(policy.delayForAttempt(4) == std::chrono::milliseconds{8000});
    CHECK(policy.delayForAttempt(5) == std::chrono::milliseconds{8000});
}

TEST_CASE("state-change worker resumes from the latest state and stops on cancellation",
          "[jmap][sync]")
{
    ensureApplication();

    FakeStateChangeSource source;
    source.queuedResults = {
        javelin::jmap::sync::StateChangeEvent{
            .newState = "state-2",
            .changedTypes = {"Mailbox", "Email"},
            .changedStates = {{"Mailbox", "m2"}, {"Email", "e2"}},
        },
        javelin::jmap::sync::StateChangeEvent{
            .newState = "state-3",
            .changedTypes = {"Email"},
            .changedStates = {},
        },
    };

    javelin::jmap::sync::StateChangeCancellation cancellation;
    FakeStateChangeConsumer consumer;
    consumer.cancellation = &cancellation;
    consumer.cancelAfterUpdates = 2;
    FakeStateChangeSleeper sleeper;

    const javelin::jmap::sync::StateChangeWorker worker{source, consumer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeSubscription(), cancellation));

    CHECK(summary.lastState == "state-3");
    CHECK(summary.successfulSubscriptions == 2);
    CHECK(summary.transientFailures == 0);
    CHECK(summary.cancelled);
    REQUIRE(consumer.updates.size() == 2);
    CHECK(consumer.updates.front().changedStates.at("Email") == "e2");
    REQUIRE(source.subscriptions.size() == 1);
    CHECK(source.subscriptions.front().lastState == "state-1");
}

TEST_CASE("state-change worker advances state without notifying for connect events", "[jmap][sync]")
{
    ensureApplication();

    FakeStateChangeSource source;
    source.queuedResults = {
        javelin::jmap::sync::StateChangeEvent{
            .newState = "state-2",
            .changedTypes = {},
            .changedStates = {},
            .notifyConsumer = false,
        },
        javelin::jmap::sync::StateChangeEvent{
            .newState = "state-3",
            .changedTypes = {"Email"},
            .changedStates = {},
        },
    };

    javelin::jmap::sync::StateChangeCancellation cancellation;
    FakeStateChangeConsumer consumer;
    consumer.cancellation = &cancellation;
    consumer.cancelAfterUpdates = 1;
    FakeStateChangeSleeper sleeper;

    const javelin::jmap::sync::StateChangeWorker worker{source, consumer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeSubscription(), cancellation));

    CHECK(summary.lastState == "state-3");
    CHECK(summary.successfulSubscriptions == 1);
    REQUIRE(consumer.updates.size() == 1);
    CHECK(consumer.updates.front().newState == "state-3");
    REQUIRE(source.subscriptions.size() == 1);
    CHECK(source.subscriptions.front().lastState == "state-1");
}

TEST_CASE("state-change worker backs off and retries after transient transport failures",
          "[jmap][sync]")
{
    ensureApplication();

    FakeStateChangeSource source;
    source.queuedResults = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "temporary failure",
            .httpStatus = std::nullopt,
        },
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
            .message = "server overload",
            .httpStatus = 503,
        },
        javelin::jmap::sync::StateChangeEvent{
            .newState = "state-2",
            .changedTypes = {"Email"},
            .changedStates = {},
        },
    };

    javelin::jmap::sync::StateChangeCancellation cancellation;
    FakeStateChangeConsumer consumer;
    consumer.cancellation = &cancellation;
    consumer.cancelAfterUpdates = 1;
    FakeStateChangeSleeper sleeper;
    std::vector<javelin::jmap::OperationErrorCode> reportedErrors;

    const javelin::jmap::sync::StateChangeWorker worker{
        source,
        consumer,
        sleeper,
        javelin::jmap::sync::BackoffPolicy{
            .initialDelay = std::chrono::milliseconds{500},
            .maxDelay = std::chrono::milliseconds{2000},
        },
        {},
        [&reportedErrors](const javelin::jmap::OperationError& error)
        { reportedErrors.push_back(error.code); }};
    const auto summary = QCoro::waitFor(worker.run(makeSubscription(), cancellation));

    CHECK(summary.successfulSubscriptions == 1);
    CHECK(summary.transientFailures == 2);
    CHECK(summary.lastState == "state-2");
    CHECK(sleeper.delays == std::vector<std::chrono::milliseconds>{
                                std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}});
    CHECK(reportedErrors == std::vector{javelin::jmap::OperationErrorCode::NetworkUnavailable,
                                        javelin::jmap::OperationErrorCode::ServerUnavailable});
}

TEST_CASE("state-change worker exits immediately when transport reports cancellation",
          "[jmap][sync]")
{
    ensureApplication();

    FakeStateChangeSource source;
    source.queuedResults = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::Cancelled,
            .message = "cancelled",
            .httpStatus = std::nullopt,
        },
    };

    javelin::jmap::sync::StateChangeCancellation cancellation;
    FakeStateChangeConsumer consumer;
    FakeStateChangeSleeper sleeper;

    const javelin::jmap::sync::StateChangeWorker worker{source, consumer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeSubscription(), cancellation));

    CHECK(summary.cancelled);
    CHECK(summary.successfulSubscriptions == 0);
    CHECK(summary.transientFailures == 0);
    CHECK(sleeper.delays.empty());
}

TEST_CASE("state-change worker stops retrying when authentication is rejected", "[jmap][sync]")
{
    ensureApplication();

    FakeStateChangeSource source;
    source.queuedResults = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::HttpFailure,
            .message = "unauthorized",
            .httpStatus = 401,
        },
    };
    javelin::jmap::sync::StateChangeCancellation cancellation;
    FakeStateChangeConsumer consumer;
    FakeStateChangeSleeper sleeper;

    const javelin::jmap::sync::StateChangeWorker worker{source, consumer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeSubscription(), cancellation));

    REQUIRE(summary.terminalError.has_value());
    CHECK(summary.terminalError->code == javelin::jmap::OperationErrorCode::AuthenticationRequired);
    CHECK(summary.transientFailures == 0);
    CHECK_FALSE(summary.cancelled);
    CHECK(sleeper.delays.empty());
    CHECK(source.subscriptions.size() == 1);
}

TEST_CASE("state-change worker reports connection status transitions", "[jmap][sync]")
{
    ensureApplication();

    FakeStateChangeSource source;
    source.queuedResults = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "temporary failure",
            .httpStatus = std::nullopt,
        },
        javelin::jmap::sync::StateChangeEvent{
            .newState = "state-2",
            .changedTypes = {"Email"},
            .changedStates = {},
        },
    };

    javelin::jmap::sync::StateChangeCancellation cancellation;
    FakeStateChangeConsumer consumer;
    consumer.cancellation = &cancellation;
    consumer.cancelAfterUpdates = 1;
    FakeStateChangeSleeper sleeper;
    std::vector<javelin::jmap::sync::StateChangeConnectionStatus> statuses;

    const javelin::jmap::sync::StateChangeWorker worker{
        source,
        consumer,
        sleeper,
        {},
        [&statuses](const javelin::jmap::sync::StateChangeConnectionStatus status)
        { statuses.push_back(status); }};
    const auto summary = QCoro::waitFor(worker.run(makeSubscription(), cancellation));

    CHECK(summary.successfulSubscriptions == 1);
    CHECK(statuses == std::vector<javelin::jmap::sync::StateChangeConnectionStatus>{
                          javelin::jmap::sync::StateChangeConnectionStatus::Connecting,
                          javelin::jmap::sync::StateChangeConnectionStatus::Disconnected,
                          javelin::jmap::sync::StateChangeConnectionStatus::Connecting,
                          javelin::jmap::sync::StateChangeConnectionStatus::Disconnected});
}
