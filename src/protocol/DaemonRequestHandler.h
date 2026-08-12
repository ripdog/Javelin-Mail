#pragma once

#include "protocol/ActionContract.h"
#include "protocol/ActivationContract.h"
#include "protocol/CacheContract.h"
#include "protocol/HandshakeContract.h"
#include "protocol/SettingsContract.h"

#include <optional>

namespace javelin::protocol
{
    class DaemonRequestHandler : public ActivationRequestHandler
    {
      public:
        ~DaemonRequestHandler() override = default;

        [[nodiscard]] virtual HandshakeReply handleHello(const HelloRequest& request) = 0;
        [[nodiscard]] virtual CommandReply handleCommand(CommandRequest request) = 0;
        [[nodiscard]] virtual MaterializationReply
        handleMaterialization(MaterializationRequest request) = 0;
        virtual void
        handleCancelMaterializationScope(const CancelMaterializationScopeRequest& request) = 0;
        [[nodiscard]] virtual SettingsReadReply
        handleGetSettings(const GetSettingsRequest& request) = 0;
        [[nodiscard]] virtual SettingsUpdateReply
        handleUpdateSettings(UpdateSettingsRequest request) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError>
        handleCacheAccessSuspended(const CacheAccessSuspendedAcknowledgement& acknowledgement) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError>
        handlePing(const PingRequest& request) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError> handleGuiReadyForActivation() = 0;
        [[nodiscard]] std::optional<BoundaryError>
        handleGuiActivation(const ActivationRoute&) override
        {
            return BoundaryError{.code = BoundaryErrorCode::UnsupportedOperation,
                                 .field = QStringLiteral("activation"),
                                 .detail = QStringLiteral("GUI activation is not supported")};
        }
    };
} // namespace javelin::protocol
