#include "app/CommandDispatcher.h"

#include <catch2/catch_test_macros.hpp>

namespace
{
    class FakeRefreshPort final : public javelin::app::AccountRefreshPort
    {
      public:
        bool requestAccountSynchronization(std::string_view) override
        {
            ++synchronizationRequests;
            return synchronizationAccepted;
        }

        QCoro::Task<javelin::jmap::LiveRefreshResult>
        bootstrapAccount(javelin::app::AccountBootstrapIntent) override
        {
            co_return javelin::jmap::OperationError{};
        }

        QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string) override
        {
            co_return javelin::jmap::OperationError{};
        }

        bool synchronizationAccepted = true;
        int synchronizationRequests = 0;
    };

    class RecordingSink final : public javelin::protocol::BoundaryEventSink
    {
      public:
        void onBoundaryEvent(const javelin::protocol::BoundaryEvent& event) override
        {
            received = event;
        }

        std::optional<javelin::protocol::BoundaryEvent> received;
    };

    [[nodiscard]] javelin::protocol::CommandRequest
    refreshRequest(const javelin::protocol::CommandId id, const bool force = true)
    {
        return {.id = id,
                .command = javelin::protocol::RefreshAccountCommand{
                    .accountId = QStringLiteral("account-1"), .force = force}};
    }
} // namespace

TEST_CASE("command dispatcher admits refresh with an epoch and affected account", "[app][protocol]")
{
    FakeRefreshPort refreshPort;
    javelin::app::CommandDispatcher dispatcher{refreshPort};
    const auto request = refreshRequest({.value = QUuid::createUuid()});

    const auto reply = dispatcher.dispatch(request);
    const auto* accepted = std::get_if<javelin::protocol::CommandAccepted>(&reply);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->epoch.value == 1);
    CHECK(accepted->affectedKeys == std::vector<QString>{QStringLiteral("account-1")});
    CHECK(refreshPort.synchronizationRequests == 1);
}

TEST_CASE("command dispatcher reports later failure separately from admission", "[app][protocol]")
{
    FakeRefreshPort refreshPort;
    javelin::app::CommandDispatcher dispatcher{refreshPort};
    RecordingSink sink;
    dispatcher.setEventSink(&sink);
    const auto reply = dispatcher.dispatch(refreshRequest({.value = QUuid::createUuid()}));
    const auto* accepted = std::get_if<javelin::protocol::CommandAccepted>(&reply);
    REQUIRE(accepted != nullptr);
    REQUIRE(accepted->operation.has_value());

    dispatcher.publishOperationFailure(*accepted->operation,
                                       {.code = javelin::protocol::BoundaryErrorCode::Busy,
                                        .field = QStringLiteral("account"),
                                        .detail = QStringLiteral("refresh was deferred")});

    REQUIRE(sink.received.has_value());
    const auto* failure = std::get_if<javelin::protocol::OperationFailed>(&*sink.received);
    REQUIRE(failure != nullptr);
    CHECK(failure->operation == *accepted->operation);
    CHECK(failure->error.code == javelin::protocol::BoundaryErrorCode::Busy);
}

TEST_CASE("command dispatcher replays the same UUID without repeating the workflow",
          "[app][protocol]")
{
    FakeRefreshPort refreshPort;
    javelin::app::CommandDispatcher dispatcher{refreshPort};
    const auto request = refreshRequest({.value = QUuid::createUuid()});

    const auto first = dispatcher.dispatch(request);
    const auto retry = dispatcher.dispatch(request);

    const auto* firstAccepted = std::get_if<javelin::protocol::CommandAccepted>(&first);
    const auto* retryAccepted = std::get_if<javelin::protocol::CommandAccepted>(&retry);
    REQUIRE(firstAccepted != nullptr);
    REQUIRE(retryAccepted != nullptr);
    CHECK(firstAccepted->id == retryAccepted->id);
    CHECK(firstAccepted->epoch == retryAccepted->epoch);
    CHECK(firstAccepted->affectedKeys == retryAccepted->affectedKeys);
    CHECK(refreshPort.synchronizationRequests == 1);
}

TEST_CASE("command dispatcher rejects UUID reuse for a different command", "[app][protocol]")
{
    FakeRefreshPort refreshPort;
    javelin::app::CommandDispatcher dispatcher{refreshPort};
    const auto id = javelin::protocol::CommandId{.value = QUuid::createUuid()};

    REQUIRE(std::holds_alternative<javelin::protocol::CommandAccepted>(
        dispatcher.dispatch(refreshRequest(id, true))));
    const auto reply = dispatcher.dispatch(refreshRequest(id, false));

    const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == javelin::protocol::BoundaryErrorCode::InvalidRequest);
    CHECK(refreshPort.synchronizationRequests == 1);
}

TEST_CASE("command dispatcher preserves direct rejection without advancing the epoch",
          "[app][protocol]")
{
    FakeRefreshPort refreshPort;
    refreshPort.synchronizationAccepted = false;
    javelin::app::CommandDispatcher dispatcher{refreshPort};

    const auto reply = dispatcher.dispatch(refreshRequest({.value = QUuid::createUuid()}));
    const auto* rejected = std::get_if<javelin::protocol::CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code ==
          javelin::protocol::BoundaryErrorCode::NoUsableAccountConfiguration);
    CHECK(dispatcher.currentEpoch().value == 0);
}
