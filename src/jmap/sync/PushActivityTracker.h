#pragma once

#include "jmap/sync/StateChangeSource.h"

#include <QElapsedTimer>
#include <QString>
#include <QUrl>

#include <chrono>

namespace javelin::jmap::sync
{
    class PushActivityTracker
    {
      public:
        PushActivityTracker(StateChangeStatusCallback statusCallback, const QUrl& endpoint,
                            std::chrono::milliseconds timeout);

        void recordActivity();
        void setTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] const QString& serverBaseUrl() const;
        [[nodiscard]] std::chrono::milliseconds timeout() const;
        [[nodiscard]] bool hasTimedOut() const;

      private:
        StateChangeStatusCallback m_statusCallback;
        QElapsedTimer m_lastActivity;
        QString m_serverBaseUrl;
        std::chrono::milliseconds m_timeout;
    };
} // namespace javelin::jmap::sync
