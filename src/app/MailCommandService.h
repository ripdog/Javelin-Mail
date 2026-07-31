#pragma once

#include "app/MailApplicationPorts.h"

namespace javelin::app
{
    class MailApplicationService;

    class MailCommandService final : public MailCommandPort
    {
      public:
        explicit MailCommandService(MailApplicationService& service);

        [[nodiscard]] QueuedMailboxSelectionMutationResult
        queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent) override;
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueDestroyMessages(std::string accountId, std::optional<std::string> sourceMailboxId,
                             MessageSelection selection) override;
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueMarkMessagesUnread(std::string accountId, std::optional<std::string> sourceMailboxId,
                                MessageSelection selection) override;
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueMarkEmailRead(std::string accountId, std::string emailId) override;
        [[nodiscard]] QueuedMessageSelectionMutationResult
        queueSetEmailFlagged(std::string accountId, std::string emailId, bool flagged) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
        submitPendingEmailMutations(
            std::string accountId,
            std::optional<std::string> operationGroupId = std::nullopt) override;

      private:
        MailApplicationService& m_service;
    };

} // namespace javelin::app
