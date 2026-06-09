#pragma once

#include "jmap/api/MethodCaller.h"
#include "jmap/cache/Database.h"
#include "jmap/sync/RefreshNotificationTypes.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    struct MailboxRefreshSummary
    {
        std::size_t representativeCount = 0;
        bool usedIncrementalRefresh = false;
        std::vector<std::string> changedEmailIds;
        std::vector<std::string> insertedEmailIds;
        std::vector<std::string> removedEmailIds;
        bool requiresNotificationScan = false;
        std::vector<RefreshNotificationCandidate> notificationCandidates;
    };

    struct MailboxRefreshError
    {
        QString message;
    };

    using MailboxRefreshResult = std::variant<MailboxRefreshSummary, MailboxRefreshError>;

    class MailboxRefreshExecutor
    {
      public:
        MailboxRefreshExecutor(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                               javelin::jmap::api::MethodCaller& methodCaller,
                               javelin::jmap::api::ApiRequestContext apiRequestContext);

        [[nodiscard]] QCoro::Task<MailboxRefreshResult>
        refreshCollapsedMailbox(std::string accountId, std::string mailboxId,
                                std::function<void(const QString&)> reportProgress) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::MethodCaller& m_methodCaller;
        javelin::jmap::api::ApiRequestContext m_apiRequestContext;
    };

} // namespace javelin::jmap::sync
