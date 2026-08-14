#pragma once

#include "protocol/ProtocolTypes.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace javelin::protocol
{
    struct ActionId
    {
        std::uint16_t value = 0;
        friend bool operator==(const ActionId&, const ActionId&) = default;
        friend auto operator<=>(const ActionId&, const ActionId&) = default;
    };

    struct RefreshAccountCommand
    {
        QString accountId;
        bool force = false;
        friend bool operator==(const RefreshAccountCommand&,
                               const RefreshAccountCommand&) = default;
    };

    struct RemoteActionCommand
    {
        ActionId action;
        QByteArray payload;
        friend bool operator==(const RemoteActionCommand&, const RemoteActionCommand&) = default;
    };

    using ApplicationCommand = std::variant<RefreshAccountCommand, RemoteActionCommand>;

    struct CommandRequest
    {
        CommandId id;
        ApplicationCommand command;
    };

    struct CommandAccepted
    {
        CommandId id;
        std::optional<OperationId> operation;
        InvalidationEpoch epoch;
        std::vector<ChangedDomain> changedDomains;
        std::vector<QString> affectedKeys;
        std::optional<QByteArray> immediateResult;
    };

    struct CommandRejected
    {
        CommandId id;
        BoundaryError error;
    };

    using CommandReply = std::variant<CommandAccepted, CommandRejected>;

    struct OperationFailed
    {
        OperationId operation;
        BoundaryError error;
    };

    struct OperationCompleted
    {
        OperationId operation;
        QByteArray result;
    };

    class CommandClient
    {
      public:
        virtual ~CommandClient() = default;
        [[nodiscard]] virtual CommandReply submitCommand(CommandRequest request) = 0;
    };
} // namespace javelin::protocol
