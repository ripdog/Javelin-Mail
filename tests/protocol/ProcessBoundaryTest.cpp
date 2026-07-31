#include "protocol/InProcessEndpoint.h"

#include <catch2/catch_test_macros.hpp>

namespace
{

    using namespace javelin::protocol;

    class RecordingHandler final : public DaemonRequestHandler
    {
      public:
        HandshakeReply handleHello(const HelloRequest&) override
        {
            return ReadyReply{.protocol = {.major = 1, .minor = 0},
                              .daemon = {.value = QUuid::createUuid()},
                              .cache = {.instance = {.value = QUuid::createUuid()},
                                        .schema = {.value = 3},
                                        .dataVersion = {.value = 7}},
                              .epoch = {.value = 12},
                              .settingsRevision = {.value = 5}};
        }

        CommandReply handleCommand(CommandRequest request) override
        {
            receivedCommand = std::move(request);
            return CommandAccepted{.id = receivedCommand->id,
                                   .operation = std::nullopt,
                                   .epoch = {.value = 12},
                                   .changedDomains = {ChangedDomain::MailQueryWindows},
                                   .affectedKeys = {QStringLiteral("account-1")}};
        }

        MaterializationReply handleMaterialization(MaterializationRequest request) override
        {
            receivedMaterialization = std::move(request);
            return MaterializationAccepted{.id = receivedMaterialization->id};
        }

        void
        handleCancelMaterializationScope(const CancelMaterializationScopeRequest& request) override
        {
            cancelledScope = request.scope;
        }

        SettingsReadReply handleGetSettings(const GetSettingsRequest&) override
        {
            return SettingsSnapshotReply{
                .snapshot = {.revision = {.value = 5},
                             .schemaVersion = 1,
                             .languageTag = QStringLiteral("en-NZ"),
                             .notificationsEnabled = true,
                             .watchedMailboxIds = {QStringLiteral("inbox")}}};
        }

        SettingsUpdateReply handleUpdateSettings(UpdateSettingsRequest request) override
        {
            receivedSettingsUpdate = std::move(request);
            return SettingsUpdated{.revision = {.value = 6}};
        }

        std::optional<BoundaryError> handleCacheAccessSuspended(
            const CacheAccessSuspendedAcknowledgement& acknowledgement) override
        {
            acknowledgedCache = acknowledgement.instance;
            return std::nullopt;
        }

        std::optional<BoundaryError> handlePing(const PingRequest&) override
        {
            pinged = true;
            return std::nullopt;
        }

        std::optional<BoundaryError> handleGuiReadyForActivation() override
        {
            guiReady = true;
            return std::nullopt;
        }

        std::optional<CommandRequest> receivedCommand;
        std::optional<MaterializationRequest> receivedMaterialization;
        std::optional<UpdateSettingsRequest> receivedSettingsUpdate;
        std::optional<ScopeId> cancelledScope;
        std::optional<CacheInstanceId> acknowledgedCache;
        bool pinged = false;
        bool guiReady = false;
    };

    class RecordingSink final : public BoundaryEventSink
    {
      public:
        void onBoundaryEvent(const BoundaryEvent& event) override
        {
            received = event;
        }

        std::optional<BoundaryEvent> received;
    };

    [[nodiscard]] CommandRequest refreshRequest()
    {
        return {.id = {.value = QUuid::createUuid()},
                .command =
                    RefreshAccountCommand{.accountId = QStringLiteral("account-1"), .force = true}};
    }

} // namespace

TEST_CASE("in-process endpoint carries typed command admission", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    const auto request = refreshRequest();
    const auto reply = endpoint.submitCommand(request);

    const auto* accepted = std::get_if<CommandAccepted>(&reply);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->id == request.id);
    CHECK(accepted->epoch.value == 12);
    REQUIRE(handler.receivedCommand.has_value());
    const auto* refresh = std::get_if<RefreshAccountCommand>(&handler.receivedCommand->command);
    REQUIRE(refresh != nullptr);
    CHECK(refresh->accountId == QStringLiteral("account-1"));
    CHECK(refresh->force);
}

TEST_CASE("invalid typed commands are rejected before daemon dispatch", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    const CommandRequest request{.id = {}, .command = RefreshAccountCommand{}};
    const auto reply = endpoint.submitCommand(request);

    const auto* rejected = std::get_if<CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->id == request.id);
    CHECK(rejected->error.code == BoundaryErrorCode::InvalidIdentifier);
    CHECK_FALSE(handler.receivedCommand.has_value());
}

TEST_CASE("materialization requests retain request and scope identity", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler};

    const MaterializationRequest request{
        .id = {.value = QUuid::createUuid()},
        .scope = {.value = QUuid::createUuid()},
        .request = MailboxWindowMaterialization{.accountId = QStringLiteral("account-1"),
                                                .mailboxId = QStringLiteral("inbox"),
                                                .offset = 40,
                                                .limit = 50}};
    const auto reply = endpoint.requestMaterialization(request);

    const auto* accepted = std::get_if<MaterializationAccepted>(&reply);
    REQUIRE(accepted != nullptr);
    CHECK(accepted->id == request.id);
    REQUIRE(handler.receivedMaterialization.has_value());
    CHECK(handler.receivedMaterialization->scope == request.scope);

    endpoint.cancelMaterializationScope(request.scope);
    REQUIRE(handler.cancelledScope.has_value());
    CHECK(*handler.cancelledScope == request.scope);
}

TEST_CASE("endpoint validates bounded values and estimates their frame size", "[protocol]")
{
    RecordingHandler handler;
    InProcessEndpoint endpoint{handler, {.maximumStringBytes = 8}};

    const auto request = refreshRequest();
    const auto reply = endpoint.submitCommand(request);
    const auto* rejected = std::get_if<CommandRejected>(&reply);
    REQUIRE(rejected != nullptr);
    CHECK(rejected->error.code == BoundaryErrorCode::ValueTooLarge);
    CHECK(estimatedEncodedSize(ClientRequest{request}) < BoundaryLimits{}.maximumFrameBytes);
}

TEST_CASE("endpoint exposes settings, handshake, lifecycle and events through typed ports",
          "[protocol]")
{
    RecordingHandler handler;
    RecordingSink sink;
    InProcessEndpoint endpoint{handler};
    REQUIRE_FALSE(endpoint.attachEventSink(sink).has_value());

    const auto handshake = endpoint.hello({.protocol = {.major = 1, .minor = 0},
                                           .build = {.application = QStringLiteral("Javelin-Mail"),
                                                     .revision = QStringLiteral("test")}});
    REQUIRE(std::get_if<ReadyReply>(&handshake) != nullptr);
    const auto settings = endpoint.getSettings();
    const auto* snapshot = std::get_if<SettingsSnapshotReply>(&settings);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->snapshot.revision.value == 5);

    CHECK_FALSE(endpoint.ping().has_value());
    CHECK(handler.pinged);
    CHECK_FALSE(endpoint.readyForActivation().has_value());
    CHECK(handler.guiReady);

    endpoint.publishEvent(CacheInvalidation{.epoch = {.value = 13},
                                            .changedDomains = {ChangedDomain::MessageMetadata},
                                            .affectedKeys = {QStringLiteral("email-1")}});
    REQUIRE(sink.received.has_value());
    const auto* invalidation = std::get_if<CacheInvalidation>(&*sink.received);
    REQUIRE(invalidation != nullptr);
    CHECK(invalidation->epoch.value == 13);

    endpoint.publishEvent(
        OperationFailed{.operation = {.value = QUuid::createUuid()},
                        .error = {.code = BoundaryErrorCode::Busy,
                                  .field = QStringLiteral("refresh"),
                                  .detail = QStringLiteral("operation was deferred")}});
    REQUIRE(sink.received.has_value());
    const auto* failure = std::get_if<OperationFailed>(&*sink.received);
    REQUIRE(failure != nullptr);
    CHECK(failure->error.code == BoundaryErrorCode::Busy);

    endpoint.detachEventSink(sink);
    const auto update = endpoint.updateSettings({.baseRevision = {.value = 5},
                                                 .update = {.languageTag = QStringLiteral("mi-NZ"),
                                                            .notificationsEnabled = std::nullopt,
                                                            .watchedMailboxIds = std::nullopt}});
    CHECK(std::holds_alternative<SettingsUpdated>(update));
    REQUIRE(handler.receivedSettingsUpdate.has_value());
    CHECK(handler.receivedSettingsUpdate->update.languageTag == QStringLiteral("mi-NZ"));
}
