#pragma once

#include "protocol/BoundaryEventContract.h"
#include "protocol/DaemonRequestHandler.h"
#include "protocol/ProtocolValidation.h"

namespace javelin::protocol
{

    class InProcessEndpoint final : public CommandClient,
                                    public MaterializationClient,
                                    public SettingsClient,
                                    public DaemonStatusClient,
                                    public ActivationClient,
                                    public CacheAccessClient
    {
      public:
        explicit InProcessEndpoint(DaemonRequestHandler& handler, BoundaryLimits limits = {});

        InProcessEndpoint(const InProcessEndpoint&) = delete;
        InProcessEndpoint& operator=(const InProcessEndpoint&) = delete;
        InProcessEndpoint(InProcessEndpoint&&) = delete;
        InProcessEndpoint& operator=(InProcessEndpoint&&) = delete;

        ~InProcessEndpoint() override = default;

        [[nodiscard]] std::optional<BoundaryError> attachEventSink(BoundaryEventSink& sink);
        void detachEventSink(BoundaryEventSink& sink);
        void publishEvent(const BoundaryEvent& event);

        [[nodiscard]] CommandReply submitCommand(CommandRequest request) override;
        [[nodiscard]] MaterializationReply
        requestMaterialization(MaterializationRequest request) override;
        void cancelMaterializationScope(ScopeId scope) override;
        [[nodiscard]] SettingsReadReply getSettings() override;
        [[nodiscard]] SettingsUpdateReply updateSettings(UpdateSettingsRequest request) override;
        [[nodiscard]] HandshakeReply hello(HelloRequest request) override;
        [[nodiscard]] std::optional<BoundaryError> ping() override;
        [[nodiscard]] std::optional<BoundaryError> readyForActivation() override;
        [[nodiscard]] std::optional<BoundaryError> acknowledgeCacheAccessSuspended(
            CacheAccessSuspendedAcknowledgement acknowledgement) override;

      private:
        [[nodiscard]] static BoundaryError invalidRequestError(const QString& field,
                                                               const QString& detail);

        DaemonRequestHandler& m_handler;
        BoundaryLimits m_limits;
        BoundaryEventSink* m_eventSink = nullptr;
    };

} // namespace javelin::protocol
