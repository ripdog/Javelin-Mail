#pragma once

#include "jmap/sync/LongPollWorker.h"

#include <QCoroTask>

#include <QPointer>
#include <QTimer>

#include <coroutine>
#include <string>

class QNetworkAccessManager;
class QNetworkReply;

namespace javelin::jmap::sync
{

    class QtStateChangeSleeper final : public StateChangeSleeper
    {
      public:
        QtStateChangeSleeper();
        ~QtStateChangeSleeper() override;

        [[nodiscard]] QCoro::Task<void> sleepFor(std::chrono::milliseconds delay) override;
        void cancel();

      private:
        class WaitAwaiter
        {
          public:
            WaitAwaiter(QtStateChangeSleeper& sleeper, std::chrono::milliseconds delay);

            [[nodiscard]] bool await_ready() const noexcept;
            void await_suspend(std::coroutine_handle<> handle);
            void await_resume() const noexcept;

          private:
            QtStateChangeSleeper& m_sleeper;
            std::chrono::milliseconds m_delay;
        };

        void beginWait(std::coroutine_handle<> handle, std::chrono::milliseconds delay);
        void resumeWaiter();

        QTimer m_timer;
        std::coroutine_handle<> m_waiter;
    };

    class EventSourceStateChangeSource final : public StateChangeSource
    {
      public:
        EventSourceStateChangeSource(QNetworkAccessManager& networkAccessManager,
                                     std::string eventSourceUrl, std::string accessToken,
                                     StateChangeStatusCallback statusCallback = {});
        ~EventSourceStateChangeSource() override;

        [[nodiscard]] QCoro::Task<StateChangeSourceResult>
        consume(StateChangeSubscription subscription, StateChangeConsumer& consumer,
                StateChangeCancellation& cancellation) override;
        void cancel() override;

      private:
        QNetworkAccessManager& m_networkAccessManager;
        std::string m_eventSourceUrl;
        std::string m_accessToken;
        StateChangeStatusCallback m_statusCallback;
        QPointer<QNetworkReply> m_activeReply;
    };

} // namespace javelin::jmap::sync
