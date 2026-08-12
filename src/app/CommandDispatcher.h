#pragma once

#include "app/AccountRefreshApplicationPorts.h"
#include "protocol/ActionContract.h"
#include "protocol/BoundaryEventContract.h"
#include "protocol/ProtocolTypes.h"
#include "protocol/ProtocolValidation.h"

#include <deque>
#include <map>

namespace javelin::app
{
    class CommandDispatcher final
    {
      public:
        explicit CommandDispatcher(AccountRefreshPort& refreshPort,
                                   protocol::BoundaryLimits limits = {});

        [[nodiscard]] protocol::CommandReply dispatch(protocol::CommandRequest request);
        [[nodiscard]] protocol::InvalidationEpoch currentEpoch() const;
        void setEventSink(protocol::BoundaryEventSink* sink) noexcept;
        void publishOperationFailure(protocol::OperationId operation,
                                     protocol::BoundaryError error) const;

      private:
        struct ReplayEntry
        {
            protocol::CommandRequest request;
            protocol::CommandReply reply;
        };

        [[nodiscard]] static bool sameCommand(const protocol::ApplicationCommand& left,
                                              const protocol::ApplicationCommand& right);
        [[nodiscard]] static QString replayKey(const protocol::CommandId& id);
        [[nodiscard]] protocol::CommandReply reject(const protocol::CommandId& id,
                                                    protocol::BoundaryError error) const;

        AccountRefreshPort& m_refreshPort;
        protocol::BoundaryLimits m_limits;
        protocol::InvalidationEpoch m_epoch;
        std::map<QString, ReplayEntry> m_replays;
        std::deque<QString> m_replayOrder;
        protocol::BoundaryEventSink* m_eventSink = nullptr;
    };
} // namespace javelin::app
