#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/LiveConnectionSettings.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::cache
{
    class DatabaseConnection;
}

namespace javelin::jmap::api
{
    class AbstractTransport;
    class JmapMethodTransport;
} // namespace javelin::jmap::api

namespace javelin::jmap
{
    struct MailCapabilityContext;

    struct LiveRefreshSummary
    {
        std::string accountId;
        std::optional<std::string> selectedMailboxId;
        std::size_t mailboxCount = 0;
        std::size_t emailCount = 0;
        std::string resolvedSessionUrl;
    };

    using LiveRefreshResult = std::variant<LiveRefreshSummary, OperationError>;

    class AccountBootstrapClient
    {
      public:
        AccountBootstrapClient(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                               javelin::jmap::api::AbstractTransport& resourceTransport,
                               javelin::jmap::api::JmapMethodTransport& methodTransport);
        ~AccountBootstrapClient();

        [[nodiscard]] QCoro::Task<LiveRefreshResult>
        bootstrap(LiveConnectionSettings settings, std::string connectionId,
                  std::function<void(const QString&)> progressCallback = {},
                  std::vector<std::string> mailboxIds = {});

      private:
        std::unique_ptr<MailCapabilityContext> m_impl;
    };

} // namespace javelin::jmap
