#pragma once

#include "protocol/BoundaryEventContract.h"
#include "protocol/DaemonRequestHandler.h"

#include <cstddef>
#include <optional>
#include <variant>

namespace javelin::protocol
{
    using ClientRequest =
        std::variant<HelloRequest, CommandRequest, MaterializationRequest,
                     CancelMaterializationScopeRequest, GetSettingsRequest, UpdateSettingsRequest,
                     CacheAccessSuspendedAcknowledgement, PingRequest>;

    [[nodiscard]] std::optional<BoundaryError> validate(const ClientRequest& request,
                                                        const BoundaryLimits& limits = {});
    [[nodiscard]] std::size_t estimatedEncodedSize(const ClientRequest& request);
    [[nodiscard]] std::size_t estimatedEncodedSize(const BoundaryEvent& event);
} // namespace javelin::protocol
