#pragma once

#include "jmap/sync/LongPollWorker.h"

#include <QCoroTask>

#include <string>

class QNetworkAccessManager;

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

        [[nodiscard]] QCoro::Task<LongPollResult> poll(const LongPollRequest& request) override;

      private:
        QNetworkAccessManager& m_networkAccessManager;
        std::string m_accessToken;
        LongPollStatusCallback m_statusCallback;
    };

} // namespace javelin::jmap::sync
