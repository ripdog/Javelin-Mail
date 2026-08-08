#include "app/MailCommandService.h"

#include "app/MailApplicationService.h"

#include <utility>

namespace javelin::app
{
    MailCommandService::MailCommandService(MailApplicationService& service) : m_service(service)
    {
    }

    QCoro::Task<QueuedMailboxSelectionMutationResult>
    MailCommandService::queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent)
    {
        co_return m_service.queueMailboxSelectionMutation(std::move(intent));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueDestroyMessages(std::string accountId,
                                             std::optional<std::string> sourceMailboxId,
                                             MessageSelection selection)
    {
        co_return m_service.queueDestroyMessages(std::move(accountId), std::move(sourceMailboxId),
                                                 std::move(selection));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueMarkMessagesUnread(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                MessageSelection selection)
    {
        co_return m_service.queueMarkMessagesUnread(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        co_return m_service.queueMarkEmailRead(std::move(accountId), std::move(emailId));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueSetEmailFlagged(std::string accountId, std::string emailId,
                                             const bool flagged)
    {
        co_return m_service.queueSetEmailFlagged(std::move(accountId), std::move(emailId), flagged);
    }

    QCoro::Task<QueuedMessageSelectionMutationResult> MailCommandService::queueSetMessagesTag(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, std::string keyword, const bool enabled)
    {
        co_return m_service.queueSetMessagesTag(std::move(accountId), std::move(sourceMailboxId),
                                                std::move(selection), std::move(keyword), enabled);
    }

    QCoro::Task<SaveMailTagDefinitionResult>
    MailCommandService::saveTagDefinition(SaveMailTagDefinition definition)
    {
        co_return m_service.saveTagDefinition(std::move(definition));
    }

    QCoro::Task<QueuedMailTagDeletionResult> MailCommandService::deleteTag(std::string accountId,
                                                                           std::string keyword)
    {
        co_return m_service.deleteTag(std::move(accountId), std::move(keyword));
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailCommandService::submitPendingEmailMutations(std::string accountId,
                                                    std::optional<std::string> operationGroupId)
    {
        return m_service.submitPendingEmailMutations(std::move(accountId),
                                                     std::move(operationGroupId));
    }

} // namespace javelin::app
