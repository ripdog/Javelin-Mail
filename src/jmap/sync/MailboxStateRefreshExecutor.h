#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/api/Session.h"
#include "jmap/cache/Database.h"

#include <QCoroTask>

#include <QString>

#include <string>
#include <variant>

namespace javelin::jmap::sync
{

    struct MailboxStateRefreshSummary
    {
        std::size_t mailboxCount = 0;
        bool usedIncrementalRefresh = false;
        bool changed = false;
        bool superseded = false;
    };

    using MailboxStateRefreshResult = std::variant<MailboxStateRefreshSummary, OperationError>;

    class MailboxStateRefreshExecutor
    {
      public:
        MailboxStateRefreshExecutor(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                    javelin::jmap::api::MethodCaller& methodCaller,
                                    javelin::jmap::api::ApiRequestContext apiRequestContext);

        [[nodiscard]] QCoro::Task<MailboxStateRefreshResult> refresh(std::string accountId) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::MethodCaller& m_methodCaller;
        javelin::jmap::api::ApiRequestContext m_apiRequestContext;
    };

} // namespace javelin::jmap::sync
