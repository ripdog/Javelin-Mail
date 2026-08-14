#pragma once

#include "app/MailApplicationPorts.h"
#include "app/MailboxSelectionMutation.h"
#include "app/undo/MailHistoryPort.h"
#include "storage/sqlite/DatabaseConnection.h"

#include <QObject>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace javelin::jmap
{
    class EmailMutationEngine;
    class MailboxMutationEngine;
    class MailQueryClient;
} // namespace javelin::jmap

namespace javelin::jmap::cache
{
    class MailboxMessageReader;
    class MailboxReader;
    class MailTagReader;
} // namespace javelin::jmap::cache

namespace javelin::app::undo
{
    class UndoManager;
}

namespace javelin::app
{
    class AccountRuntimeManager;
    class ApplicationErrorCoordinator;
    class MailboxMaintenanceRegistry;
    class ThreadMaterializationCoordinator;
    class WorkScheduler;

    class MailMutationApplicationService final : public QObject,
                                                 public javelin::app::undo::MailHistoryPort
    {
        Q_OBJECT

      public:
        MailMutationApplicationService(
            javelin::jmap::cache::DatabaseConnection& databaseConnection,
            javelin::jmap::EmailMutationEngine& emailMutationEngine,
            javelin::jmap::MailboxMutationEngine& mailboxMutationEngine,
            javelin::jmap::MailQueryClient& queryClient,
            javelin::jmap::cache::MailboxReader& mailboxReader,
            javelin::jmap::cache::MailTagReader& mailTagReader,
            javelin::jmap::cache::MailboxMessageReader& mailboxMessageReader,
            AccountRuntimeManager& accountRuntime, ApplicationErrorCoordinator& errorCoordinator,
            WorkScheduler& workScheduler, MailboxMaintenanceRegistry& mailboxMaintenanceRegistry,
            javelin::app::undo::UndoManager& undoManager, QObject* parent = nullptr);

        void setThreadMaterializationCoordinator(ThreadMaterializationCoordinator* coordinator);
        void accountConfigured(std::string accountId);
        void networkBecameReachable();

        [[nodiscard]] QCoro::Task<QueuedMailboxSelectionMutationResult>
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection);
        [[nodiscard]] QueuedMessageSelectionMutationResult queueMarkEmailRead(std::string accountId,
                                                                              std::string emailId);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesFlagged(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, bool flagged);
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesTag(std::string accountId, std::optional<std::string> sourceMailboxId,
                            MessageSelection selection, std::string keyword, bool enabled);
        [[nodiscard]] SaveMailTagDefinitionResult
        saveTagDefinition(SaveMailTagDefinition definition);
        [[nodiscard]] QueuedMailTagDeletionResult deleteTag(std::string accountId,
                                                            std::string keyword);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
        setMailboxSubscribed(std::string accountId, std::string mailboxId, bool subscribed);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxCreateResult>
        createMailbox(std::string accountId, std::string name);
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxDestroyResult>
        destroyMailbox(std::string accountId, std::string mailboxId);

        [[nodiscard]] javelin::jmap::QueuedEmailMutationResult
        queueExactEmailMutation(std::string accountId,
                                javelin::jmap::EmailMailboxMutation mutation) override;
        [[nodiscard]] javelin::jmap::QueuedEmailMutationsResult queueExactEmailMutations(
            std::string accountId,
            std::vector<javelin::jmap::EmailMailboxMutation> mutations) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(
            std::string accountId,
            std::optional<std::string> operationGroupId = std::nullopt) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::AuthoritativeEmailsResult>
        getAuthoritativeEmails(std::string accountId, std::vector<std::string> emailIds) override;
        [[nodiscard]] javelin::jmap::AuthoritativeEmailsResult
        getEffectiveEmails(std::string_view accountId,
                           std::span<const std::string> emailIds) override;

      Q_SIGNALS:
        void cacheCommitted(javelin::app::MailCacheChange change);

      private:
        enum class SelectedMessageMutation
        {
            Destroy,
            MarkUnread,
        };

        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::OperationError>>
        ensureMessageSelectionMaterialized(std::string accountId,
                                           std::optional<std::string> sourceMailboxId,
                                           MessageSelection selection);
        [[nodiscard]] QueuedMailboxSelectionMutationResult
        queueResolvedMailboxSelectionMutation(MailboxSelectionMutationIntent intent);
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSelectedMessageMutation(std::string accountId,
                                     std::optional<std::string> sourceMailboxId,
                                     MessageSelection selection, SelectedMessageMutation mutation);
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSetMessagesKeyword(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, std::string keyword, bool enabled,
                                QString historyVerb, bool appendKeywordToHistoryLabel);
        void schedulePendingEmailMutationReplay(std::string accountId);
        void scheduleMailboxMutationReconciliation(std::string accountId);
        [[nodiscard]] QCoro::Task<void> reconcileMailboxMutations(std::string accountId);
        void scheduleTagDeletionPump();
        void pumpTagDeletions();
        [[nodiscard]] QCoro::Task<void> runTagDeletion(std::string jobId, std::string accountId,
                                                       std::string keyword);

        javelin::jmap::cache::DatabaseConnection& m_databaseConnection;
        javelin::jmap::EmailMutationEngine& m_emailMutationEngine;
        javelin::jmap::MailboxMutationEngine& m_mailboxMutationEngine;
        javelin::jmap::MailQueryClient& m_queryClient;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        javelin::jmap::cache::MailTagReader& m_mailTagReader;
        javelin::jmap::cache::MailboxMessageReader& m_mailboxMessageReader;
        AccountRuntimeManager& m_accountRuntime;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        MailboxMaintenanceRegistry& m_mailboxMaintenanceRegistry;
        ThreadMaterializationCoordinator* m_threadMaterializationCoordinator = nullptr;
        javelin::app::undo::UndoManager& m_undoManager;
        std::unordered_set<std::string> m_pendingMutationReplaysInFlight;
        std::unordered_set<std::string> m_mailboxMutationReconciliationsInFlight;
        std::unordered_set<std::string> m_runningTagDeletions;
        bool m_tagDeletionPumpScheduled = false;
    };

} // namespace javelin::app
