#pragma once

#include "jmap/sync/LongPollWorker.h"

#include <QCoroTask>

#include <QPointer>

#include <string>

class QNetworkAccessManager;
class QNetworkReply;

namespace javelin::jmap::sync
{

    class QtStateChangeSleeper final : public StateChangeSleeper
    {
      public:
        [[nodiscard]] QCoro::Task<void> sleepFor(std::chrono::milliseconds delay) override;
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
