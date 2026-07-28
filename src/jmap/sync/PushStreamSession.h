#pragma once

#include "jmap/sync/PushProtocol.h"

#include <QCoroTask>

#include <chrono>
#include <string>
#include <variant>

namespace javelin::jmap::sync
{
    struct PushStreamIgnored
    {
    };

    struct PushStreamPing
    {
        std::chrono::seconds interval;
    };

    struct PushStreamProtocolFailure
    {
        std::string message;
    };

    using PushStreamOutcome =
        std::variant<PushStreamIgnored, PushStreamPing, PushStreamProtocolFailure>;

    class PushStreamSession
    {
      public:
        PushStreamSession(StateChangeSubscription subscription, StateChangeConsumer& consumer);

        [[nodiscard]] const StateChangeSubscription& subscription() const;
        [[nodiscard]] const StateChangeStreamSummary& summary() const;
        [[nodiscard]] QCoro::Task<PushStreamOutcome> accept(PushMessage message);

      private:
        StateChangeSubscription m_subscription;
        StateChangeConsumer& m_consumer;
        StateChangeStreamSummary m_summary;
    };

} // namespace javelin::jmap::sync
