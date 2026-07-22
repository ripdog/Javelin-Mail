#include "jmap/sync/EventSourceLongPoll.h"

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

TEST_CASE("event source ping intervals accept numeric and decimal string values",
          "[jmap][push][event-source]")
{
    const auto numeric = javelin::jmap::sync::parseEventSourcePingInterval(R"({"interval":30})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(numeric));
    CHECK(std::get<std::optional<std::chrono::seconds>>(numeric) == 30s);

    const auto string = javelin::jmap::sync::parseEventSourcePingInterval(R"({"interval":"30"})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(string));
    CHECK(std::get<std::optional<std::chrono::seconds>>(string) == 30s);

    const auto milliseconds =
        javelin::jmap::sync::parseEventSourcePingInterval(R"({"interval":"30000"})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(milliseconds));
    CHECK(std::get<std::optional<std::chrono::seconds>>(milliseconds) == 30s);
}

TEST_CASE("event source ping interval strings remain strict decimal values",
          "[jmap][push][event-source]")
{
    const auto malformed =
        javelin::jmap::sync::parseEventSourcePingInterval(R"({"interval":"30 seconds"})");
    REQUIRE(std::holds_alternative<std::string>(malformed));

    const auto absent = javelin::jmap::sync::parseEventSourcePingInterval(R"({})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(absent));
    CHECK_FALSE(std::get<std::optional<std::chrono::seconds>>(absent).has_value());
}
