#pragma once

#include "jmap/api/Error.h"

#include <QCoroTask>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    struct LongPollRequest
    {
        std::string accountId;
        std::string eventSourceUrl;
        std::string lastState;
    };

    struct LongPollResponse
    {
        std::string newState;
        std::vector<std::string> changedTypes;
    };

    class LongPollCancellation
    {
      public:
        void cancel();
        [[nodiscard]] bool isCancelled() const;

      private:
        bool m_cancelled = false;
    };

    struct BackoffPolicy
    {
        std::chrono::milliseconds initialDelay{1000};
        std::chrono::milliseconds maxDelay{30000};

        [[nodiscard]] std::chrono::milliseconds
        delayForAttempt(std::size_t consecutiveFailures) const;
    };

    using LongPollResult = std::variant<LongPollResponse, javelin::jmap::api::TransportError>;

    class AbstractLongPollChannel
    {
      public:
        virtual ~AbstractLongPollChannel() = default;

        [[nodiscard]] virtual QCoro::Task<LongPollResult> poll(const LongPollRequest& request) = 0;
    };

    class AbstractLongPollObserver
    {
      public:
        virtual ~AbstractLongPollObserver() = default;

        virtual void onUpdate(const LongPollResponse& response) = 0;
    };

    class AbstractLongPollSleeper
    {
      public:
        virtual ~AbstractLongPollSleeper() = default;

        [[nodiscard]] virtual QCoro::Task<void> sleepFor(std::chrono::milliseconds delay) = 0;
    };

    struct LongPollRunSummary
    {
        std::string lastState;
        std::size_t successfulPolls = 0;
        std::size_t transientFailures = 0;
        bool cancelled = false;
    };

    class LongPollWorker
    {
      public:
        LongPollWorker(AbstractLongPollChannel& channel, AbstractLongPollObserver& observer,
                       AbstractLongPollSleeper& sleeper, BackoffPolicy backoffPolicy = {});

        [[nodiscard]] QCoro::Task<LongPollRunSummary> run(LongPollRequest request,
                                                          LongPollCancellation& cancellation) const;

      private:
        AbstractLongPollChannel& m_channel;
        AbstractLongPollObserver& m_observer;
        AbstractLongPollSleeper& m_sleeper;
        BackoffPolicy m_backoffPolicy;
    };

} // namespace javelin::jmap::sync
