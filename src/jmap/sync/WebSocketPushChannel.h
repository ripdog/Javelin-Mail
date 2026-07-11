#pragma once

#include "jmap/sync/LongPollWorker.h"

class QWebSocket;

namespace javelin::jmap::sync
{
    class WebSocketPushChannel final : public AbstractLongPollChannel
    {
      public:
        WebSocketPushChannel(std::string url, std::string accessToken,
                             LongPollStatusCallback statusCallback = {});
        ~WebSocketPushChannel() override;

        [[nodiscard]] QCoro::Task<LongPollResult> poll(LongPollRequest request,
                                                       AbstractLongPollObserver& observer,
                                                       LongPollCancellation& cancellation) override;
        void cancel() override;

      private:
        std::string m_url;
        std::string m_accessToken;
        LongPollStatusCallback m_statusCallback;
        QWebSocket* m_activeSocket = nullptr;
    };
} // namespace javelin::jmap::sync
