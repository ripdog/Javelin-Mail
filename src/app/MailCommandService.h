#pragma once

#include "app/MailApplicationPorts.h"

namespace javelin::app
{
    class MailMutationApplicationService;
    class MailTransferCommandService;

    class MailCommandService final : public MailCommandPort
    {
      public:
        MailCommandService(MailMutationApplicationService& service,
                           MailTransferCommandService& transferService);

        [[nodiscard]] QCoro::Task<QueuedMailboxSelectionMutationResult>
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent) override;
        [[nodiscard]] QCoro::Task<MailTransferExecutionResult>
        transferAcrossAccounts(CrossAccountMailTransferIntent intent) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueMarkEmailRead(std::string accountId, std::string emailId) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesFlagged(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection, bool flagged) override;
        [[nodiscard]] QCoro::Task<QueuedMessageSelectionMutationResult>
        queueSetMessagesTag(std::string accountId, std::optional<std::string> sourceMailboxId,
                            MessageSelection selection, std::string keyword, bool enabled) override;
        [[nodiscard]] QCoro::Task<SaveMailTagDefinitionResult>
        saveTagDefinition(SaveMailTagDefinition definition) override;
        [[nodiscard]] QCoro::Task<QueuedMailTagDeletionResult>
        deleteTag(std::string accountId, std::string keyword) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
        setMailboxSubscribed(std::string accountId, std::string mailboxId,
                             bool subscribed) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxCreateResult>
        createMailbox(std::string accountId, std::string name) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::MailboxDestroyResult>
        destroyMailbox(std::string accountId, std::string mailboxId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(
            std::string accountId,
            std::optional<std::string> operationGroupId = std::nullopt) override;

      private:
        MailMutationApplicationService& m_service;
        MailTransferCommandService& m_transferService;
    };

} // namespace javelin::app
