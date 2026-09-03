#include "jmap/sync/PushActivityTracker.h"
#include "jmap/sync/PushProtocol.h"
#include "jmap/sync/PushStreamSession.h"

#include <QCoroTask>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace std::chrono_literals;

namespace
{
    class RecordingConsumer final : public javelin::jmap::sync::StateChangeConsumer
    {
      public:
        [[nodiscard]] QCoro::Task<void>
        onStateChange(javelin::jmap::sync::StateChangeEvent event) override
        {
            events.push_back(std::move(event));
            co_return;
        }

        [[nodiscard]] QCoro::Task<void>
        onCalendarAlert(javelin::jmap::sync::CalendarAlertEvent event) override
        {
            alerts.push_back(std::move(event));
            co_return;
        }

        std::vector<javelin::jmap::sync::StateChangeEvent> events;
        std::vector<javelin::jmap::sync::CalendarAlertEvent> alerts;
    };
} // namespace

TEST_CASE("event source ping intervals accept numeric and decimal string values",
          "[jmap][push][event-source]")
{
    const auto numeric = javelin::jmap::sync::parsePushPingInterval(R"({"interval":30})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(numeric));
    CHECK(std::get<std::optional<std::chrono::seconds>>(numeric) == 30s);

    const auto string = javelin::jmap::sync::parsePushPingInterval(R"({"interval":"30"})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(string));
    CHECK(std::get<std::optional<std::chrono::seconds>>(string) == 30s);

    const auto milliseconds = javelin::jmap::sync::parsePushPingInterval(R"({"interval":"30000"})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(milliseconds));
    CHECK(std::get<std::optional<std::chrono::seconds>>(milliseconds) == 30s);
}

TEST_CASE("event source ping interval strings remain strict decimal values",
          "[jmap][push][event-source]")
{
    const auto malformed =
        javelin::jmap::sync::parsePushPingInterval(R"({"interval":"30 seconds"})");
    REQUIRE(std::holds_alternative<std::string>(malformed));

    const auto absent = javelin::jmap::sync::parsePushPingInterval(R"({})");
    REQUIRE(std::holds_alternative<std::optional<std::chrono::seconds>>(absent));
    CHECK_FALSE(std::get<std::optional<std::chrono::seconds>>(absent).has_value());
}

TEST_CASE("event source protocol parser returns typed pings", "[jmap][push][http]")
{
    const auto parsed =
        javelin::jmap::sync::parseEventSourcePushMessage({}, {}, "ping", {}, R"({"interval":30})");

    REQUIRE(std::holds_alternative<javelin::jmap::sync::PushPing>(parsed));
    CHECK(std::get<javelin::jmap::sync::PushPing>(parsed).interval == 30s);
    CHECK(javelin::jmap::sync::pushActivityTimeout(30s) == 75s);
}

TEST_CASE("event source protocol parser returns subscribed calendar alerts",
          "[jmap][push][event-source][calendar]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "mail-account",
        .lastState = "push-state-1",
        .types = {"CalendarAlert"},
        .groupwareAccountIds = {"calendar-account"},
    };
    const auto parsed = javelin::jmap::sync::parseEventSourcePushMessage(
        subscription, subscription.lastState, "calendarAlert", "push-state-2",
        R"({"@type":"CalendarAlert","accountId":"calendar-account","calendarEventId":"event-1","uid":"uid-1","recurrenceId":"2026-09-03T09:00:00","alertId":"alert-1"})");

    REQUIRE(std::holds_alternative<javelin::jmap::sync::CalendarAlertEvent>(parsed));
    const auto& alert = std::get<javelin::jmap::sync::CalendarAlertEvent>(parsed);
    CHECK(alert.accountId == "calendar-account");
    CHECK(alert.calendarEventId == "event-1");
    REQUIRE(alert.recurrenceId.has_value());
    CHECK(*alert.recurrenceId == "2026-09-03T09:00:00");
    CHECK(alert.alertId == "alert-1");
}

TEST_CASE("push stream sessions own state delivery for every transport", "[jmap][push]")
{
    RecordingConsumer consumer;
    javelin::jmap::sync::PushStreamSession stream{
        {
            .accountId = "account-1",
            .lastState = "state-1",
            .types = {"Email"},
            .groupwareAccountIds = {},
        },
        consumer,
    };

    auto outcome = QCoro::waitFor(stream.accept(javelin::jmap::sync::PushPing{.interval = 30s}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::PushStreamPing>(outcome));
    CHECK(std::get<javelin::jmap::sync::PushStreamPing>(outcome).interval == 30s);

    outcome = QCoro::waitFor(
        stream.accept(javelin::jmap::sync::PushProtocolError{.message = "invalid push payload"}));
    REQUIRE(std::holds_alternative<javelin::jmap::sync::PushStreamProtocolFailure>(outcome));
    CHECK(std::get<javelin::jmap::sync::PushStreamProtocolFailure>(outcome).message ==
          "invalid push payload");

    outcome = QCoro::waitFor(stream.accept(javelin::jmap::sync::StateChangeEvent{
        .newState = "state-2",
        .changedTypes = {},
        .changedStates = {},
        .notifyConsumer = false,
    }));
    CHECK(std::holds_alternative<javelin::jmap::sync::PushStreamIgnored>(outcome));
    CHECK(stream.subscription().lastState == "state-2");
    CHECK(stream.summary().lastState == "state-2");
    CHECK(stream.summary().updateCount == 0);

    outcome = QCoro::waitFor(stream.accept(javelin::jmap::sync::StateChangeEvent{
        .newState = "state-3",
        .changedTypes = {"Email"},
        .changedStates = {{"account-1", {{"Email", "email-3"}}}},
        .notifyConsumer = true,
    }));
    CHECK(std::holds_alternative<javelin::jmap::sync::PushStreamIgnored>(outcome));
    CHECK(stream.subscription().lastState == "state-3");
    CHECK(stream.summary().lastState == "state-3");
    CHECK(stream.summary().updateCount == 1);
    REQUIRE(consumer.events.size() == 1);
    CHECK(consumer.events.front().newState == "state-3");

    outcome = QCoro::waitFor(stream.accept(javelin::jmap::sync::CalendarAlertEvent{
        .accountId = "calendar-account",
        .calendarEventId = "event-1",
        .uid = "uid-1",
        .recurrenceId = std::nullopt,
        .alertId = "alert-1",
    }));
    CHECK(std::holds_alternative<javelin::jmap::sync::PushStreamIgnored>(outcome));
    CHECK(stream.subscription().lastState == "state-3");
    CHECK(stream.summary().lastState == "state-3");
    CHECK(stream.summary().updateCount == 2);
    REQUIRE(consumer.alerts.size() == 1);
    CHECK(consumer.alerts.front().calendarEventId == "event-1");
}

TEST_CASE("push activity tracking shares status and timeout state", "[jmap][push]")
{
    std::size_t connectedReports = 0;
    bool indicatorConnected = false;
    javelin::jmap::sync::PushActivityTracker activity{
        [&connectedReports,
         &indicatorConnected](const javelin::jmap::sync::StateChangeConnectionStatus status)
        {
            CHECK(status == javelin::jmap::sync::StateChangeConnectionStatus::Connected);
            ++connectedReports;
            indicatorConnected = true;
            if (connectedReports == 1)
            {
                indicatorConnected = false;
            }
        },
        QUrl{QStringLiteral("https://user:secret@mail.example.com:8443/jmap/events?ping=30")},
        350s,
    };

    activity.recordActivity();
    activity.recordActivity();
    activity.setTimeout(75s);

    CHECK(connectedReports == 2);
    CHECK(indicatorConnected);
    CHECK(activity.serverBaseUrl() == QStringLiteral("https://mail.example.com:8443"));
    CHECK(activity.timeout() == 75s);
    CHECK_FALSE(activity.hasTimedOut());
}
