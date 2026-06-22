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

    class FakeLongPollChannel final : public javelin::jmap::sync::AbstractLongPollChannel
    {
      public:
        std::vector<javelin::jmap::sync::LongPollRequest> requests;
        std::vector<std::variant<javelin::jmap::sync::LongPollResponse,
                                 javelin::jmap::api::TransportError>>
            queuedResults;

        [[nodiscard]] QCoro::Task<javelin::jmap::sync::LongPollResult>
        poll(javelin::jmap::sync::LongPollRequest request,
             javelin::jmap::sync::AbstractLongPollObserver& observer,
             javelin::jmap::sync::LongPollCancellation& cancellation) override
        {
            requests.push_back(request);
            REQUIRE_FALSE(queuedResults.empty());

            javelin::jmap::sync::LongPollStreamSummary summary{
                .lastState = request.lastState,
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

                auto response = std::get<javelin::jmap::sync::LongPollResponse>(std::move(result));
                summary.lastState = response.newState;
                if (response.notifyObserver)
                {
                    ++summary.updateCount;
                    co_await observer.onUpdate(std::move(response));
                }
            }

            co_return summary;
        }
    };

    class FakeLongPollObserver final : public javelin::jmap::sync::AbstractLongPollObserver
    {
      public:
        std::vector<javelin::jmap::sync::LongPollResponse> updates;
        javelin::jmap::sync::LongPollCancellation* cancellation = nullptr;
        std::optional<std::size_t> cancelAfterUpdates;

        [[nodiscard]] QCoro::Task<void>
        onUpdate(javelin::jmap::sync::LongPollResponse response) override
        {
            updates.push_back(response);
            if (cancellation != nullptr && cancelAfterUpdates.has_value() &&
                updates.size() >= *cancelAfterUpdates)
            {
                cancellation->cancel();
            }
            co_return;
        }
    };

    class FakeLongPollSleeper final : public javelin::jmap::sync::AbstractLongPollSleeper
    {
      public:
        std::vector<std::chrono::milliseconds> delays;

        [[nodiscard]] QCoro::Task<void> sleepFor(const std::chrono::milliseconds delay) override
        {
            delays.push_back(delay);
            co_return;
        }
    };

    [[nodiscard]] javelin::jmap::sync::LongPollRequest makeRequest()
    {
        return javelin::jmap::sync::LongPollRequest{
            .accountId = "account-1",
            .eventSourceUrl = "https://mail.example.com/jmap/eventsource",
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

TEST_CASE("long poll worker resumes from the latest state and stops on cancellation",
          "[jmap][sync]")
{
    ensureApplication();

    FakeLongPollChannel channel;
    channel.queuedResults = {
        javelin::jmap::sync::LongPollResponse{
            .newState = "state-2",
            .changedTypes = {"Mailbox", "Email"},
        },
        javelin::jmap::sync::LongPollResponse{
            .newState = "state-3",
            .changedTypes = {"Email"},
        },
    };

    javelin::jmap::sync::LongPollCancellation cancellation;
    FakeLongPollObserver observer;
    observer.cancellation = &cancellation;
    observer.cancelAfterUpdates = 2;
    FakeLongPollSleeper sleeper;

    const javelin::jmap::sync::LongPollWorker worker{channel, observer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeRequest(), cancellation));

    CHECK(summary.lastState == "state-3");
    CHECK(summary.successfulPolls == 2);
    CHECK(summary.transientFailures == 0);
    CHECK(summary.cancelled);
    REQUIRE(channel.requests.size() == 1);
    CHECK(channel.requests.front().lastState == "state-1");
}

TEST_CASE("long poll worker advances state without notifying for connect events", "[jmap][sync]")
{
    ensureApplication();

    FakeLongPollChannel channel;
    channel.queuedResults = {
        javelin::jmap::sync::LongPollResponse{
            .newState = "state-2",
            .changedTypes = {},
            .notifyObserver = false,
        },
        javelin::jmap::sync::LongPollResponse{
            .newState = "state-3",
            .changedTypes = {"Email"},
        },
    };

    javelin::jmap::sync::LongPollCancellation cancellation;
    FakeLongPollObserver observer;
    observer.cancellation = &cancellation;
    observer.cancelAfterUpdates = 1;
    FakeLongPollSleeper sleeper;

    const javelin::jmap::sync::LongPollWorker worker{channel, observer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeRequest(), cancellation));

    CHECK(summary.lastState == "state-3");
    CHECK(summary.successfulPolls == 1);
    REQUIRE(observer.updates.size() == 1);
    CHECK(observer.updates.front().newState == "state-3");
    REQUIRE(channel.requests.size() == 1);
    CHECK(channel.requests.front().lastState == "state-1");
}

TEST_CASE("long poll worker backs off and retries after transient transport failures",
          "[jmap][sync]")
{
    ensureApplication();

    FakeLongPollChannel channel;
    channel.queuedResults = {
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
        javelin::jmap::sync::LongPollResponse{
            .newState = "state-2",
            .changedTypes = {"Email"},
        },
    };

    javelin::jmap::sync::LongPollCancellation cancellation;
    FakeLongPollObserver observer;
    observer.cancellation = &cancellation;
    observer.cancelAfterUpdates = 1;
    FakeLongPollSleeper sleeper;

    const javelin::jmap::sync::LongPollWorker worker{
        channel, observer, sleeper,
        javelin::jmap::sync::BackoffPolicy{
            .initialDelay = std::chrono::milliseconds{500},
            .maxDelay = std::chrono::milliseconds{2000},
        }};
    const auto summary = QCoro::waitFor(worker.run(makeRequest(), cancellation));

    CHECK(summary.successfulPolls == 1);
    CHECK(summary.transientFailures == 2);
    CHECK(summary.lastState == "state-2");
    CHECK(sleeper.delays == std::vector<std::chrono::milliseconds>{
                                std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}});
}

TEST_CASE("long poll worker exits immediately when transport reports cancellation", "[jmap][sync]")
{
    ensureApplication();

    FakeLongPollChannel channel;
    channel.queuedResults = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::Cancelled,
            .message = "cancelled",
            .httpStatus = std::nullopt,
        },
    };

    javelin::jmap::sync::LongPollCancellation cancellation;
    FakeLongPollObserver observer;
    FakeLongPollSleeper sleeper;

    const javelin::jmap::sync::LongPollWorker worker{channel, observer, sleeper};
    const auto summary = QCoro::waitFor(worker.run(makeRequest(), cancellation));

    CHECK(summary.cancelled);
    CHECK(summary.successfulPolls == 0);
    CHECK(summary.transientFailures == 0);
    CHECK(sleeper.delays.empty());
}

TEST_CASE("long poll worker reports connection status transitions", "[jmap][sync]")
{
    ensureApplication();

    FakeLongPollChannel channel;
    channel.queuedResults = {
        javelin::jmap::api::TransportError{
            .code = javelin::jmap::api::TransportErrorCode::NetworkFailure,
            .message = "temporary failure",
            .httpStatus = std::nullopt,
        },
        javelin::jmap::sync::LongPollResponse{
            .newState = "state-2",
            .changedTypes = {"Email"},
        },
    };

    javelin::jmap::sync::LongPollCancellation cancellation;
    FakeLongPollObserver observer;
    observer.cancellation = &cancellation;
    observer.cancelAfterUpdates = 1;
    FakeLongPollSleeper sleeper;
    std::vector<javelin::jmap::sync::LongPollConnectionStatus> statuses;

    const javelin::jmap::sync::LongPollWorker worker{
        channel, observer, sleeper, {},
        [&statuses](const javelin::jmap::sync::LongPollConnectionStatus status)
        {
            statuses.push_back(status);
        }};
    const auto summary = QCoro::waitFor(worker.run(makeRequest(), cancellation));

    CHECK(summary.successfulPolls == 1);
    CHECK(statuses == std::vector<javelin::jmap::sync::LongPollConnectionStatus>{
                          javelin::jmap::sync::LongPollConnectionStatus::Connecting,
                          javelin::jmap::sync::LongPollConnectionStatus::Disconnected,
                          javelin::jmap::sync::LongPollConnectionStatus::Connecting,
                          javelin::jmap::sync::LongPollConnectionStatus::Disconnected});
}
