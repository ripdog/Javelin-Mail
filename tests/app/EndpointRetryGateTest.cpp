#include "app/account/EndpointRetryGate.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("endpoint retry gate applies exponential backoff and admits one recovery probe")
{
    using Gate = javelin::app::EndpointRetryGate;
    auto now = Gate::Clock::time_point{};
    Gate gate{{.initialDelay = 1s, .maxDelay = 8s, .probePollInterval = 250ms},
              [&now] { return now; }};

    CHECK(gate.acquire("https://mail.example.test/jmap").allowed);
    gate.recordFailure("https://mail.example.test/jmap");

    auto decision = gate.acquire("https://mail.example.test/jmap");
    CHECK_FALSE(decision.allowed);
    CHECK(decision.retryAfter == 1s);

    now += 1s;
    decision = gate.acquire("https://mail.example.test/jmap");
    CHECK(decision.allowed);
    CHECK(decision.probe);

    const auto concurrent = gate.acquire("https://mail.example.test/jmap");
    CHECK_FALSE(concurrent.allowed);
    CHECK(concurrent.retryAfter == 250ms);

    gate.recordFailure("https://mail.example.test/jmap");
    decision = gate.acquire("https://mail.example.test/jmap");
    CHECK_FALSE(decision.allowed);
    CHECK(decision.retryAfter == 2s);

    now += 2s;
    REQUIRE(gate.acquire("https://mail.example.test/jmap").allowed);
    gate.recordFailure("https://mail.example.test/jmap");
    now += 4s;
    REQUIRE(gate.acquire("https://mail.example.test/jmap").allowed);
    gate.recordFailure("https://mail.example.test/jmap");
    now += 8s;
    REQUIRE(gate.acquire("https://mail.example.test/jmap").allowed);
    gate.recordFailure("https://mail.example.test/jmap");

    CHECK(gate.acquire("https://mail.example.test/jmap").retryAfter == 8s);
}

TEST_CASE("endpoint retry gate coalesces concurrent failures in the same cooldown window")
{
    using Gate = javelin::app::EndpointRetryGate;
    auto now = Gate::Clock::time_point{};
    Gate gate{{.initialDelay = 1s, .maxDelay = 1min, .probePollInterval = 250ms},
              [&now] { return now; }};

    gate.recordFailure("https://mail.example.test/jmap");
    now += 100ms;
    gate.recordFailure("https://mail.example.test/jmap");
    now += 100ms;
    gate.recordFailure("https://mail.example.test/jmap");

    const auto decision = gate.acquire("https://mail.example.test/jmap");
    CHECK_FALSE(decision.allowed);
    CHECK(decision.retryAfter == 800ms);

    now += 800ms;
    REQUIRE(gate.acquire("https://mail.example.test/jmap").allowed);
    gate.recordFailure("https://mail.example.test/jmap");
    CHECK(gate.acquire("https://mail.example.test/jmap").retryAfter == 2s);
}

TEST_CASE("late failures from an initial request burst do not advance the retry exponent")
{
    using Gate = javelin::app::EndpointRetryGate;
    auto now = Gate::Clock::time_point{};
    Gate gate{{.initialDelay = 1s, .maxDelay = 1min, .probePollInterval = 250ms},
              [&now] { return now; }};

    gate.recordFailure("https://mail.example.test/jmap");
    now += 1500ms;
    gate.recordFailure("https://mail.example.test/jmap");

    const auto probe = gate.acquire("https://mail.example.test/jmap");
    REQUIRE(probe.allowed);
    REQUIRE(probe.probe);
    gate.recordFailure("https://mail.example.test/jmap");
    CHECK(gate.acquire("https://mail.example.test/jmap").retryAfter == 2s);
}

TEST_CASE("endpoint retry gate isolates independent endpoints")
{
    using Gate = javelin::app::EndpointRetryGate;
    auto now = Gate::Clock::time_point{};
    Gate gate{{.initialDelay = 1s, .maxDelay = 1min, .probePollInterval = 250ms},
              [&now] { return now; }};

    gate.recordFailure("https://mail.example.test/jmap");
    CHECK_FALSE(gate.acquire("https://mail.example.test/jmap").allowed);
    CHECK(gate.acquire("https://calendar.example.test/jmap").allowed);
}

TEST_CASE("endpoint retry gate honors Retry-After and resets immediately on success")
{
    using Gate = javelin::app::EndpointRetryGate;
    auto now = Gate::Clock::time_point{};
    Gate gate{{.initialDelay = 1s, .maxDelay = 1min, .probePollInterval = 250ms},
              [&now] { return now; }};

    gate.recordFailure("https://mail.example.test/jmap", 45s);
    CHECK(gate.acquire("https://mail.example.test/jmap").retryAfter == 45s);

    gate.recordSuccess("https://mail.example.test/jmap");
    const auto first = gate.acquire("https://mail.example.test/jmap");
    const auto second = gate.acquire("https://mail.example.test/jmap");
    CHECK(first.allowed);
    CHECK_FALSE(first.probe);
    CHECK(second.allowed);
    CHECK_FALSE(second.probe);
}
