#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/MethodCaller.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <optional>
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
        // A recovered Email state gap cannot prove query membership for Emails outside the local
        // working set. The coordinator must reconcile its tracked mailbox query windows without
        // treating those presentation queries as owners of the account Email cursor.
        bool mailboxQueriesNeedReconciliation = false;
        bool notificationEventsCreated = false;
        bool notificationBaselineEstablished = false;
        bool superseded = false;
        std::vector<std::string> changedMailboxIds;
        std::vector<std::string> queryAffectedMailboxIds;
        std::vector<std::string> insertedEmailIds;
    };

    using MailDeltaRefreshResult = std::variant<MailDeltaRefreshSummary, OperationError>;

    // Owns account-wide Mailbox/Email object-state progression. In particular, the global Email
    // sync token may advance here only after every locally relevant Email represented by that
    // transition has been reconciled (or explicitly accounted for during rebaseline recovery).
    class MailDeltaRefreshExecutor
    {
      public:
        MailDeltaRefreshExecutor(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                 javelin::jmap::api::MethodCaller& methodCaller,
                                 javelin::jmap::api::ApiRequestContext apiRequestContext);

        [[nodiscard]] QCoro::Task<MailDeltaRefreshResult>
        refresh(std::string accountId, MailDeltaRefreshRequest request,
                std::string remoteAccountId = {},
                std::optional<std::vector<std::string>> notificationBaselineMailboxIds =
                    std::nullopt) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::MethodCaller& m_methodCaller;
        javelin::jmap::api::ApiRequestContext m_apiRequestContext;
    };

} // namespace javelin::jmap::sync
