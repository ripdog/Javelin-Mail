#include "jmap/sync/LongPollWorker.h"

#include <QDebug>

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
                                   const BackoffPolicy backoffPolicy,
                                   LongPollStatusCallback statusCallback)
        : m_channel(channel), m_observer(observer), m_sleeper(sleeper),
          m_backoffPolicy(backoffPolicy), m_statusCallback(std::move(statusCallback))
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
            if (m_statusCallback)
            {
                m_statusCallback(LongPollConnectionStatus::Connecting);
            }

            qInfo().noquote() << "Long poll worker polling"
                              << QString::fromStdString(request.accountId)
                              << QString::fromStdString(request.lastState);

            const auto result = co_await m_channel.poll(request);
            if (std::holds_alternative<javelin::jmap::api::TransportError>(result))
            {
                const auto& error = std::get<javelin::jmap::api::TransportError>(result);
                qWarning().noquote() << "Long poll worker transport error"
                                     << static_cast<int>(error.code)
                                     << QString::fromStdString(error.message)
                                     << error.httpStatus.value_or(0);
                if (error.code == javelin::jmap::api::TransportErrorCode::Cancelled)
                {
                    if (m_statusCallback)
                    {
                        m_statusCallback(LongPollConnectionStatus::Disconnected);
                    }
                    summary.cancelled = true;
                    break;
                }

                if (m_statusCallback)
                {
                    m_statusCallback(LongPollConnectionStatus::Disconnected);
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
            qInfo().noquote() << "Long poll worker received response"
                              << QString::fromStdString(response.newState)
                              << static_cast<qulonglong>(response.changedTypes.size());
            request.lastState = response.newState;
            summary.lastState = response.newState;
            ++summary.successfulPolls;
            co_await m_observer.onUpdate(response);
        }

        if (cancellation.isCancelled())
        {
            if (m_statusCallback)
            {
                m_statusCallback(LongPollConnectionStatus::Disconnected);
            }
            summary.cancelled = true;
        }

        co_return summary;
    }

} // namespace javelin::jmap::sync
