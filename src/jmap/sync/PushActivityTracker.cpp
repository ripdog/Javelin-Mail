#include "jmap/sync/PushActivityTracker.h"

#include <utility>

namespace javelin::jmap::sync
{
    namespace
    {
        [[nodiscard]] QString makeServerBaseUrl(const QUrl& endpoint)
        {
            QUrl baseUrl;
            baseUrl.setScheme(endpoint.scheme());
            baseUrl.setHost(endpoint.host());
            if (const int port = endpoint.port(); port >= 0)
                baseUrl.setPort(port);
            return baseUrl.toString(QUrl::FullyEncoded);
        }
    } // namespace

    PushActivityTracker::PushActivityTracker(StateChangeStatusCallback statusCallback,
                                             const QUrl& endpoint,
                                             const std::chrono::milliseconds timeout)
        : m_statusCallback(std::move(statusCallback)), m_serverBaseUrl(makeServerBaseUrl(endpoint)),
          m_timeout(timeout)
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

    const QString& PushActivityTracker::serverBaseUrl() const
    {
        return m_serverBaseUrl;
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
