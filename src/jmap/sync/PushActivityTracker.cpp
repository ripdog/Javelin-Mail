#include "jmap/sync/PushActivityTracker.h"

#include <utility>

namespace javelin::jmap::sync
{
    PushActivityTracker::PushActivityTracker(StateChangeStatusCallback statusCallback,
                                             const std::chrono::milliseconds timeout)
        : m_statusCallback(std::move(statusCallback)), m_timeout(timeout)
    {
        m_lastActivity.start();
    }

    void PushActivityTracker::recordActivity()
    {
        m_lastActivity.restart();
        if (m_statusCallback)
            m_statusCallback(StateChangeConnectionStatus::Connected);
    }

    void PushActivityTracker::setTimeout(const std::chrono::milliseconds timeout)
    {
        m_timeout = timeout;
    }

    std::chrono::milliseconds PushActivityTracker::timeout() const
    {
        return m_timeout;
    }

    bool PushActivityTracker::hasTimedOut() const
    {
        return m_lastActivity.elapsed() > m_timeout.count();
    }
} // namespace javelin::jmap::sync
