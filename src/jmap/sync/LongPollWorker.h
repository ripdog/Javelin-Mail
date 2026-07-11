#pragma once

#include "jmap/api/Error.h"

#include <QCoroTask>

#include <chrono>
#include <functional>
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
        std::vector<std::string> types;
    };

    struct LongPollResponse
    {
        std::string newState;
        std::vector<std::string> changedTypes;
        bool notifyObserver = true;
    };

    struct LongPollStreamSummary
    {
        std::string lastState;
        std::size_t updateCount = 0;
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

    using LongPollResult = std::variant<LongPollStreamSummary, javelin::jmap::api::TransportError>;

    enum class LongPollConnectionStatus
    {
        Disconnected,
        Connecting,
        Connected,
    };

    using LongPollStatusCallback = std::function<void(LongPollConnectionStatus)>;

    class AbstractLongPollObserver
    {
      public:
        virtual ~AbstractLongPollObserver() = default;

        [[nodiscard]] virtual QCoro::Task<void> onUpdate(LongPollResponse response) = 0;
    };

    class AbstractLongPollChannel
    {
      public:
        virtual ~AbstractLongPollChannel() = default;
        virtual void cancel()
        {
        }

        [[nodiscard]] virtual QCoro::Task<LongPollResult>
        poll(LongPollRequest request, AbstractLongPollObserver& observer,
             LongPollCancellation& cancellation) = 0;
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
                       AbstractLongPollSleeper& sleeper, BackoffPolicy backoffPolicy = {},
                       LongPollStatusCallback statusCallback = {});

        [[nodiscard]] QCoro::Task<LongPollRunSummary> run(LongPollRequest request,
                                                          LongPollCancellation& cancellation) const;

      private:
        AbstractLongPollChannel& m_channel;
        AbstractLongPollObserver& m_observer;
        AbstractLongPollSleeper& m_sleeper;
        BackoffPolicy m_backoffPolicy;
        LongPollStatusCallback m_statusCallback;
    };

} // namespace javelin::jmap::sync
