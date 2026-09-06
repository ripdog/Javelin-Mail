#pragma once

#include "jmap/OperationError.h"
#include "jmap/api/MethodCaller.h"
#include "jmap/sync/MailCommitEffects.h"
#include "jmap/sync/MutationJournal.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QCoroTask>

#include <QString>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::sync
{

    struct MailboxRefreshWindowSummary
    {
        std::size_t position = 0;
        std::size_t returnedLimit = 0;
        std::optional<std::size_t> total;
        std::string queryState;
        std::size_t representativeCount = 0;
    };

    struct MailboxRefreshSummary
    {
        std::size_t representativeCount = 0;
        std::optional<MailboxRefreshWindowSummary> canonicalWindow;
        bool usedIncrementalRefresh = false;
        bool canonicalWindowMaterialized = false;
        bool requiresFullRefresh = false;
        bool superseded = false;
        std::vector<std::string> changedEmailIds;
        std::vector<std::string> insertedEmailIds;
        std::vector<std::string> removedEmailIds;
        MailCommitEffects effects;
    };

    using MailboxRefreshResult = std::variant<MailboxRefreshSummary, OperationError>;

    [[nodiscard]] std::optional<OperationError>
    rebaseActiveEmailProjections(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                 std::string_view accountId, std::vector<std::string> emailIds,
                                 std::string_view serverState,
                                 MailCommitEffects* reconciliationEffects = nullptr);
    [[nodiscard]] std::optional<OperationError>
    rebaseActiveEmailProjections(MutationProjectionTransaction& transaction,
                                 javelin::jmap::cache::DatabaseConnection& databaseConnection,
                                 std::string_view accountId, std::vector<std::string> emailIds,
                                 std::string_view serverState,
                                 MailCommitEffects* reconciliationEffects = nullptr);

    // Owns mailbox query/window state only: Email/queryChanges, ordered membership and bounded
    // materialization. Email/get state observed while filling a window is not authority for the
    // account-wide Email sync token and must never advance that cursor.
    class MailboxRefreshExecutor
    {
      public:
        MailboxRefreshExecutor(javelin::jmap::cache::DatabaseConnection& databaseConnection,
                               javelin::jmap::api::MethodCaller& methodCaller,
                               javelin::jmap::api::ApiRequestContext apiRequestContext);

        [[nodiscard]] QCoro::Task<MailboxRefreshResult>
        refreshCollapsedMailbox(std::string accountId, std::string mailboxId,
                                std::function<void(const QString&)> reportProgress,
                                bool forceFullRefresh = false,
                                std::string remoteAccountId = {}) const;

      private:
        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::api::MethodCaller& m_methodCaller;
        javelin::jmap::api::ApiRequestContext m_apiRequestContext;
    };

} // namespace javelin::jmap::sync
