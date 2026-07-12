#include "jmap/api/Cancellation.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("cancellation callbacks run once and observe cancelled state", "[jmap][cancellation]")
{
    javelin::jmap::api::CancellationSource source;
    const auto token = source.token();
    int callbackCount = 0;
    const auto registration = token.registerCallback([&callbackCount]() { ++callbackCount; });
    static_cast<void>(registration);

    CHECK_FALSE(token.isCancellationRequested());
    source.cancel();
    source.cancel();

    CHECK(token.isCancellationRequested());
    CHECK(callbackCount == 1);
}

TEST_CASE("released cancellation registrations are not invoked", "[jmap][cancellation]")
{
    javelin::jmap::api::CancellationSource source;
    int callbackCount = 0;
    auto registration = source.token().registerCallback([&callbackCount]() { ++callbackCount; });
    registration.reset();

    source.cancel();
    CHECK(callbackCount == 0);
}

TEST_CASE("callbacks registered after cancellation run immediately", "[jmap][cancellation]")
{
    javelin::jmap::api::CancellationSource source;
    source.cancel();

    int callbackCount = 0;
    const auto registration =
        source.token().registerCallback([&callbackCount]() { ++callbackCount; });
    static_cast<void>(registration);

    CHECK(callbackCount == 1);
}
