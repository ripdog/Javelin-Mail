#include "jmap/sync/WebSocketPushChannel.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("WebSocket push enable uses the requested data types", "[jmap][push][websocket]")
{
    const auto encoded = javelin::jmap::sync::encodeWebSocketPushEnable({
        .accountId = "account-1",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox", "Calendar", "CalendarEvent"},
    });

    REQUIRE(encoded.has_value());
    CHECK(encoded->find(R"("@type":"WebSocketPushEnable")") != std::string::npos);
    CHECK(encoded->find(R"("dataTypes":["Email","Mailbox","Calendar","CalendarEvent"])") !=
          std::string::npos);
    CHECK(encoded->find(R"("pushState":"push-state-1")") != std::string::npos);
}
