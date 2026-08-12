#pragma once

#include "protocol/ActivationContract.h"
#include "protocol/SocketSecurity.h"

#include <optional>
#include <variant>

namespace javelin::protocol
{
    using SocketActivationResult = std::variant<std::optional<BoundaryError>, SocketTransportError>;

    class SocketActivationClient final
    {
      public:
        [[nodiscard]] static SocketActivationResult request(const SocketClientOptions& options,
                                                            ActivationRoute route);
    };
} // namespace javelin::protocol
