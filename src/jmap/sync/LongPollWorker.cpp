#include "jmap/sync/LongPollWorker.h"

#include <algorithm>

namespace javelin::jmap::sync
{

    void LongPollCancellation::cancel()
    {
        m_cancelled = true;
    }

    bool LongPollCancellation::isCancelled() const
    {
        return m_cancelled;
    }

    std::chrono::milliseconds
    BackoffPolicy::delayForAttempt(const std::size_t consecutiveFailures) const
    {
        if (consecutiveFailures == 0)
        {
            return std::chrono::milliseconds{0};
        }

        auto delay = initialDelay;
        for (std::size_t attempt = 1; attempt < consecutiveFailures; ++attempt)
        {
            delay = std::min(delay * 2, maxDelay);
        }

        return std::min(delay, maxDelay);
    }

    LongPollWorker::LongPollWorker(AbstractLongPollChannel& channel,
                                   AbstractLongPollObserver& observer,
                                   AbstractLongPollSleeper& sleeper,
                                   const BackoffPolicy backoffPolicy)
        : m_channel(channel), m_observer(observer), m_sleeper(sleeper),
          m_backoffPolicy(backoffPolicy)
    {
    }

    QCoro::Task<LongPollRunSummary> LongPollWorker::run(LongPollRequest request,
                                                        LongPollCancellation& cancellation) const
    {
        LongPollRunSummary summary{
            .lastState = request.lastState,
            .successfulPolls = 0,
            .transientFailures = 0,
            .cancelled = false,
        };

        std::size_t consecutiveFailures = 0;
        while (!cancellation.isCancelled())
        {
            const auto result = co_await m_channel.poll(request);
            if (std::holds_alternative<javelin::jmap::api::TransportError>(result))
            {
                const auto& error = std::get<javelin::jmap::api::TransportError>(result);
                if (error.code == javelin::jmap::api::TransportErrorCode::Cancelled)
                {
                    summary.cancelled = true;
                    break;
                }

                ++summary.transientFailures;
                ++consecutiveFailures;
                const auto delay = m_backoffPolicy.delayForAttempt(consecutiveFailures);
                if (delay.count() > 0)
                {
                    co_await m_sleeper.sleepFor(delay);
                }
                continue;
            }

            consecutiveFailures = 0;
            const auto& response = std::get<LongPollResponse>(result);
            request.lastState = response.newState;
            summary.lastState = response.newState;
            ++summary.successfulPolls;
            m_observer.onUpdate(response);
        }

        if (cancellation.isCancelled())
        {
            summary.cancelled = true;
        }

        co_return summary;
    }

} // namespace javelin::jmap::sync
