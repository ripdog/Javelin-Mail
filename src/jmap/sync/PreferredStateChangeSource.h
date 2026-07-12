#pragma once

#include "jmap/sync/StateChangeSource.h"

#include <memory>
#include <string>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::sync
{
    class PreferredStateChangeSource final : public StateChangeSource
    {
      public:
        PreferredStateChangeSource(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            std::string accountId, std::string webSocketUrl,
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
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        std::string m_accountId;
        std::string m_webSocketUrl;
        std::unique_ptr<StateChangeSource> m_webSocketSource;
        std::unique_ptr<StateChangeSource> m_httpFallbackSource;
    };

} // namespace javelin::jmap::sync
