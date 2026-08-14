#pragma once

#include "protocol/CacheContract.h"
#include "protocol/ProtocolTypes.h"

#include <QString>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::protocol
{
    struct HelloRequest
    {
        ProtocolVersion protocol;
        BuildIdentity build;
    };

    struct ReadyReply
    {
        ProtocolVersion protocol;
        DaemonInstanceId daemon;
        CacheIdentity cache;
        QString cacheDatabasePath;
        InvalidationEpoch epoch;
        SettingsRevision settingsRevision;
    };

    struct HandshakeRejected
    {
        BoundaryError error;
    };

    using HandshakeReply = std::variant<ReadyReply, HandshakeRejected>;

    enum class AccountState : std::uint8_t
    {
        Unknown,
        Ready,
        Synchronizing,
        AuthenticationRequired,
        Failed,
        Paused,
    };

    struct AccountStatus
    {
        QString accountId;
        AccountState state = AccountState::Unknown;
        QString detail;
    };

    enum class DaemonLifecycle : std::uint8_t
    {
        Starting,
        Ready,
        Recovering,
        ShuttingDown,
    };

    struct DaemonStatus
    {
        DaemonLifecycle lifecycle = DaemonLifecycle::Starting;
        std::vector<AccountStatus> accounts;
    };

    struct PingRequest
    {
    };

    class DaemonStatusClient
    {
      public:
        virtual ~DaemonStatusClient() = default;
        [[nodiscard]] virtual HandshakeReply hello(HelloRequest request) = 0;
        [[nodiscard]] virtual std::optional<BoundaryError> ping() = 0;
    };
} // namespace javelin::protocol
