#include "jmap/sync/LongPollWorker.h"

#include <QDebug>

#include <algorithm>

namespace javelin::jmap::sync
{

    void StateChangeCancellation::cancel()
    {
        m_cancelled = true;
    }

    bool StateChangeCancellation::isCancelled() const
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

    StateChangeWorker::StateChangeWorker(StateChangeSource& source, StateChangeConsumer& consumer,
                                         StateChangeSleeper& sleeper,
                                         const BackoffPolicy backoffPolicy,
                                         StateChangeStatusCallback statusCallback,
                                         StateChangeErrorCallback errorCallback)
        : m_source(source), m_consumer(consumer), m_sleeper(sleeper),
          m_backoffPolicy(backoffPolicy), m_statusCallback(std::move(statusCallback)),
          m_errorCallback(std::move(errorCallback))
    {
    }

    QCoro::Task<StateChangeRunSummary>
    StateChangeWorker::run(StateChangeSubscription subscription,
                           StateChangeCancellation& cancellation) const
    {
        StateChangeRunSummary summary{
            .lastState = subscription.lastState,
            .successfulSubscriptions = 0,
            .transientFailures = 0,
            .cancelled = false,
            .terminalError = std::nullopt,
        };

        std::size_t consecutiveFailures = 0;
        while (!cancellation.isCancelled())
        {
            if (m_statusCallback)
            {
                m_statusCallback(StateChangeConnectionStatus::Connecting);
            }

            const auto result = co_await m_source.consume(subscription, m_consumer, cancellation);
            if (std::holds_alternative<javelin::jmap::api::TransportError>(result))
            {
                const auto& error = std::get<javelin::jmap::api::TransportError>(result);
                qWarning().noquote()
                    << "State-change source transport error" << static_cast<int>(error.code)
                    << QString::fromStdString(error.message) << error.httpStatus.value_or(0);
                if (error.code == javelin::jmap::api::TransportErrorCode::Cancelled)
                {
                    if (m_statusCallback)
                    {
                        m_statusCallback(StateChangeConnectionStatus::Disconnected);
                    }
                    summary.cancelled = true;
                    break;
                }

                const auto classified = javelin::jmap::operationError(error);
                if (m_errorCallback)
                    m_errorCallback(classified);
                if (!javelin::jmap::isTransientError(classified))
                {
                    if (m_statusCallback)
                        m_statusCallback(StateChangeConnectionStatus::Disconnected);
                    summary.terminalError = classified;
                    break;
                }

                if (m_statusCallback)
                {
                    m_statusCallback(StateChangeConnectionStatus::Disconnected);
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
            const auto& streamSummary = std::get<StateChangeStreamSummary>(result);
            subscription.lastState = streamSummary.lastState;
            summary.lastState = streamSummary.lastState;
            summary.successfulSubscriptions += streamSummary.updateCount;
        }

        if (cancellation.isCancelled())
        {
            if (m_statusCallback)
            {
                m_statusCallback(StateChangeConnectionStatus::Disconnected);
            }
            summary.cancelled = true;
        }

        co_return summary;
    }

} // namespace javelin::jmap::sync
