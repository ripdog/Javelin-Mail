#pragma once

#include "jmap/sync/StateChangeSource.h"

#include <QCoroTask>

#include <chrono>

namespace javelin::jmap::sync
{

    struct BackoffPolicy
    {
        std::chrono::milliseconds initialDelay{1000};
        std::chrono::milliseconds maxDelay{30000};

        [[nodiscard]] std::chrono::milliseconds
        delayForAttempt(std::size_t consecutiveFailures) const;
    };

    class StateChangeSleeper
    {
      public:
        virtual ~StateChangeSleeper() = default;

        [[nodiscard]] virtual QCoro::Task<void> sleepFor(std::chrono::milliseconds delay) = 0;
    };

    struct StateChangeRunSummary
    {
        std::string lastState;
        std::size_t successfulSubscriptions = 0;
        std::size_t transientFailures = 0;
        bool cancelled = false;
    };

    class StateChangeWorker
    {
      public:
        StateChangeWorker(StateChangeSource& source, StateChangeConsumer& consumer,
                          StateChangeSleeper& sleeper, BackoffPolicy backoffPolicy = {},
                          StateChangeStatusCallback statusCallback = {});

        [[nodiscard]] QCoro::Task<StateChangeRunSummary>
        run(StateChangeSubscription subscription, StateChangeCancellation& cancellation) const;

      private:
        StateChangeSource& m_source;
        StateChangeConsumer& m_consumer;
        StateChangeSleeper& m_sleeper;
        BackoffPolicy m_backoffPolicy;
        StateChangeStatusCallback m_statusCallback;
    };

} // namespace javelin::jmap::sync
