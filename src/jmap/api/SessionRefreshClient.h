#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"

#include <QCoroTask>

#include <memory>
#include <string>
#include <variant>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class AbstractTransport;
}

namespace javelin::jmap
{
    struct MailCapabilityContext;

    struct SessionRefreshSummary
    {
        std::string ownerAccountId;
        std::string resolvedSessionUrl;
        bool websocketAdvertised = false;
        bool websocketPushSupported = false;
    };

    using SessionRefreshResult = std::variant<SessionRefreshSummary, OperationError>;

    class SessionRefreshClient
    {
      public:
        SessionRefreshClient(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                             javelin::jmap::api::AbstractTransport& resourceTransport);
        ~SessionRefreshClient();

        [[nodiscard]] QCoro::Task<SessionRefreshResult> refresh(LiveConnectionSettings settings,
                                                                std::string connectionId,
                                                                std::string ownerAccountId,
                                                                std::string ownerRemoteAccountId);

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
