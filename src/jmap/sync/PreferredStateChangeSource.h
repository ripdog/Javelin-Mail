#pragma once

#include "jmap/api/JmapMethodTransport.h"
#include "jmap/sync/StateChangeSource.h"

#include <memory>
#include <string>

namespace javelin::jmap::sync
{
    class PreferredStateChangeSource final : public StateChangeSource
    {
      public:
        PreferredStateChangeSource(javelin::jmap::api::WebSocketFailureCooldowns& cooldowns,
                                   std::string webSocketUrl,
                                   std::unique_ptr<StateChangeSource> webSocketSource,
                                   std::unique_ptr<StateChangeSource> httpFallbackSource);
        ~PreferredStateChangeSource() override;

        PreferredStateChangeSource(const PreferredStateChangeSource&) = delete;
        PreferredStateChangeSource& operator=(const PreferredStateChangeSource&) = delete;
        PreferredStateChangeSource(PreferredStateChangeSource&&) = delete;
        PreferredStateChangeSource& operator=(PreferredStateChangeSource&&) = delete;

        void cancel() override;

        [[nodiscard]] QCoro::Task<StateChangeSourceResult>
        consume(StateChangeSubscription subscription, StateChangeConsumer& consumer,
                StateChangeCancellation& cancellation) override;

      private:
        javelin::jmap::api::WebSocketFailureCooldowns& m_cooldowns;
        std::string m_webSocketUrl;
        std::unique_ptr<StateChangeSource> m_webSocketSource;
        std::unique_ptr<StateChangeSource> m_httpFallbackSource;
    };

} // namespace javelin::jmap::sync
