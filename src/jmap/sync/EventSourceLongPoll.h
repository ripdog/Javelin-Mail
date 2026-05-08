#pragma once

#include "jmap/sync/LongPollWorker.h"

#include <QCoroTask>

#include <QPointer>

#include <string>

class QNetworkAccessManager;
class QNetworkReply;

namespace javelin::jmap::sync
{

    class QtLongPollSleeper final : public AbstractLongPollSleeper
    {
      public:
        [[nodiscard]] QCoro::Task<void> sleepFor(std::chrono::milliseconds delay) override;
    };

    class EventSourceLongPollChannel final : public AbstractLongPollChannel
    {
      public:
        EventSourceLongPollChannel(QNetworkAccessManager& networkAccessManager,
                                   std::string accessToken,
                                   LongPollStatusCallback statusCallback = {});
        ~EventSourceLongPollChannel() override;

        [[nodiscard]] QCoro::Task<LongPollResult> poll(const LongPollRequest& request) override;
        void cancel();

      private:
        QNetworkAccessManager& m_networkAccessManager;
        std::string m_accessToken;
        LongPollStatusCallback m_statusCallback;
        QPointer<QNetworkReply> m_activeReply;
    };

} // namespace javelin::jmap::sync
