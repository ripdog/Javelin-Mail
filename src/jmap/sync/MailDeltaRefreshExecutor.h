#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/cache/Database.h"

#include <QCoroTask>

#include <string>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    struct MailDeltaRefreshRequest
    {
        bool mailbox = false;
        bool email = false;
    };

    struct MailDeltaRefreshSummary
    {
        bool mailboxChanged = false;
        bool emailChanged = false;
        bool mailboxNeedsFullRefresh = false;
        bool emailNeedsFullRefresh = false;
        bool superseded = false;
        std::vector<std::string> changedMailboxIds;
        std::vector<std::string> queryAffectedMailboxIds;
        std::vector<std::string> insertedEmailIds;
    };

    using MailDeltaRefreshResult = std::variant<MailDeltaRefreshSummary, OperationError>;

    class MailDeltaRefreshExecutor
    {
      public:
        MailDeltaRefreshExecutor(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                 javelin::jmap::api::MethodCaller& methodCaller,
                                 javelin::jmap::api::ApiRequestContext apiRequestContext);

        [[nodiscard]] QCoro::Task<MailDeltaRefreshResult>
        refresh(std::string accountId, MailDeltaRefreshRequest request) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::MethodCaller& m_methodCaller;
        javelin::jmap::api::ApiRequestContext m_apiRequestContext;
    };

} // namespace javelin::jmap::sync
