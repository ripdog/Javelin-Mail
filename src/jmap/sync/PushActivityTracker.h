#pragma once

#include "jmap/sync/StateChangeSource.h"

#include <QElapsedTimer>

#include <chrono>

namespace javelin::jmap::sync
{
    class PushActivityTracker
    {
      public:
        PushActivityTracker(StateChangeStatusCallback statusCallback,
                            std::chrono::milliseconds timeout);

        void recordActivity();
        void setTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] std::chrono::milliseconds timeout() const;
        [[nodiscard]] bool hasTimedOut() const;

      private:
        StateChangeStatusCallback m_statusCallback;
        QElapsedTimer m_lastActivity;
        std::chrono::milliseconds m_timeout;
    };
} // namespace javelin::jmap::sync
