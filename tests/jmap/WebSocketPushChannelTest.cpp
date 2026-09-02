#include "jmap/sync/WebSocketPushChannel.h"

#include "jmap/sync/PushProtocol.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

namespace
{
    void ensureApplication()
    {
        if (QCoreApplication::instance() != nullptr)
            return;

        static int argc = 1;
        static char appName[] = "javelin-tests";
        static char* argv[] = {appName, nullptr};
        static QCoreApplication application(argc, argv);
        Q_UNUSED(application);
    }

    class NoopStateChangeConsumer final : public javelin::jmap::sync::StateChangeConsumer
    {
      public:
        [[nodiscard]] QCoro::Task<void>
        onStateChange(javelin::jmap::sync::StateChangeEvent) override
        {
            co_return;
        }
    };
} // namespace

TEST_CASE("WebSocket push enable uses the requested data types", "[jmap][push][websocket]")
{
    const auto encoded = javelin::jmap::sync::encodeWebSocketPushEnable({
        .accountId = "account-1",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox", "Calendar", "CalendarEvent"},
        .groupwareAccountIds = {},
    });

    REQUIRE(encoded.has_value());
    CHECK(encoded->find(R"("@type":"WebSocketPushEnable")") != std::string::npos);
    CHECK(encoded->find(R"("dataTypes":["Email","Mailbox","Calendar","CalendarEvent"])") !=
          std::string::npos);
    CHECK(encoded->find(R"("pushState":"push-state-1")") != std::string::npos);
}

TEST_CASE("WebSocket push cancellation interrupts an in-flight handshake",
          "[jmap][push][websocket]")
{
    ensureApplication();

    QTcpServer server;
    REQUIRE(server.listen(QHostAddress::LocalHost, 0));
    const auto endpoint = QStringLiteral("ws://127.0.0.1:%1/jmap").arg(server.serverPort());

    javelin::jmap::sync::WebSocketStateChangeSource source{endpoint.toStdString(), "token"};
    javelin::jmap::sync::StateChangeCancellation cancellation;
    NoopStateChangeConsumer consumer;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer::singleShot(25,
                       [&cancellation, &source]
                       {
                           cancellation.cancel();
                           source.cancel();
                       });

    const auto result = QCoro::waitFor(source.consume(
        {.accountId = "account-1", .lastState = {}, .types = {"Email"}, .groupwareAccountIds = {}},
        consumer, cancellation));

    REQUIRE(std::holds_alternative<javelin::jmap::api::TransportError>(result));
    CHECK(std::get<javelin::jmap::api::TransportError>(result).code ==
          javelin::jmap::api::TransportErrorCode::Cancelled);
    CHECK(elapsed.elapsed() < 1000);
}

TEST_CASE("state-change routing includes groupware changes from secondary accounts", "[jmap][push]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "mail-account",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox", "AddressBook", "ContactCard"},
        .groupwareAccountIds = {"mail-account", "contacts-account"},
    };
    const javelin::jmap::sync::AccountTypeStateMap changed{
        {"mail-account", {{"Email", "mail-2"}, {"Mailbox", "boxes-2"}}},
        {"contacts-account", {{"ContactCard", "contacts-2"}}},
        {"unrelated-account", {{"ContactCard", "unrelated-2"}, {"Email", "other-mail-2"}}},
    };

    CHECK(javelin::jmap::sync::subscribedStateChanges(subscription, changed) ==
          javelin::jmap::sync::AccountTypeStateMap{
              {"mail-account", {{"Email", "mail-2"}, {"Mailbox", "boxes-2"}}},
              {"contacts-account", {{"ContactCard", "contacts-2"}}}});
}

TEST_CASE("WebSocket push messages parse calendar alerts without advancing push state",
          "[jmap][push][websocket][calendar]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "mail-account",
        .lastState = "push-state-1",
        .types = {"CalendarAlert"},
        .groupwareAccountIds = {"calendar-account"},
    };
    const auto parsed = javelin::jmap::sync::parseWebSocketPushMessage(
        subscription, subscription.lastState,
        R"({"@type":"CalendarAlert","accountId":"calendar-account","calendarEventId":"event-1","uid":"uid-1","recurrenceId":null,"alertId":"alert-1"})");

    REQUIRE(std::holds_alternative<javelin::jmap::sync::CalendarAlertEvent>(parsed));
    const auto& alert = std::get<javelin::jmap::sync::CalendarAlertEvent>(parsed);
    CHECK(alert.accountId == "calendar-account");
    CHECK(alert.calendarEventId == "event-1");
    CHECK_FALSE(alert.recurrenceId.has_value());
    CHECK(alert.alertId == "alert-1");
}

TEST_CASE("WebSocket push messages use the shared protocol parser", "[jmap][push][websocket]")
{
    const javelin::jmap::sync::StateChangeSubscription subscription{
        .accountId = "account-1",
        .lastState = "push-state-1",
        .types = {"Email", "Mailbox"},
        .groupwareAccountIds = {},
    };
    auto parsed = javelin::jmap::sync::parseWebSocketPushMessage(
        subscription, subscription.lastState,
        R"({"@type":"StateChange","changed":{"account-1":{"Email":"email-2"}},"pushState":"push-state-2"})");

    REQUIRE(std::holds_alternative<javelin::jmap::sync::StateChangeEvent>(parsed));
    const auto& event = std::get<javelin::jmap::sync::StateChangeEvent>(parsed);
    CHECK(event.newState == "push-state-2");
    CHECK(event.changedTypes == std::vector<std::string>{"Email"});
    CHECK(event.changedStates ==
          javelin::jmap::sync::AccountTypeStateMap{{"account-1", {{"Email", "email-2"}}}});
}
