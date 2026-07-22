#pragma once

#include "jmap/sync/StateChangeSource.h"

class QWebSocket;

namespace javelin::jmap::sync
{
    class WebSocketStateChangeSource final : public StateChangeSource
    {
      public:
        WebSocketStateChangeSource(std::string url, std::string accessToken,
                                   StateChangeStatusCallback statusCallback = {});
        ~WebSocketStateChangeSource() override;

        [[nodiscard]] QCoro::Task<StateChangeSourceResult>
        consume(StateChangeSubscription subscription, StateChangeConsumer& consumer,
                StateChangeCancellation& cancellation) override;
        void cancel() override;

      private:
        void reportConnectedActivity() const;

        std::string m_url;
        std::string m_accessToken;
        StateChangeStatusCallback m_statusCallback;
        QWebSocket* m_activeSocket = nullptr;
    };
} // namespace javelin::jmap::sync
