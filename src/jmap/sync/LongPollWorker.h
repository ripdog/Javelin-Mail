#pragma once

#include "jmap/OperationError.h"
#include "jmap/sync/StateChangeSource.h"

#include <QCoroTask>

#include <chrono>
#include <optional>

namespace javelin::jmap::sync
{

    using StateChangeErrorCallback = std::function<void(const javelin::jmap::OperationError&)>;

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
        std::optional<javelin::jmap::OperationError> terminalError;
    };

    class StateChangeWorker
    {
      public:
        StateChangeWorker(StateChangeSource& source, StateChangeConsumer& consumer,
                          StateChangeSleeper& sleeper, BackoffPolicy backoffPolicy = {},
                          StateChangeStatusCallback statusCallback = {},
                          StateChangeErrorCallback errorCallback = {});

        [[nodiscard]] QCoro::Task<StateChangeRunSummary>
        run(StateChangeSubscription subscription, StateChangeCancellation& cancellation) const;

      private:
        StateChangeSource& m_source;
        StateChangeConsumer& m_consumer;
        StateChangeSleeper& m_sleeper;
        BackoffPolicy m_backoffPolicy;
        StateChangeStatusCallback m_statusCallback;
        StateChangeErrorCallback m_errorCallback;
    };

} // namespace javelin::jmap::sync
