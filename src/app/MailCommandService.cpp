#include "app/MailCommandService.h"

#include "app/MailApplicationService.h"

#include <utility>

namespace javelin::app
{
    MailCommandService::MailCommandService(MailApplicationService& service) : m_service(service)
    {
    }

    QueuedMailboxSelectionMutationResult
    MailCommandService::queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent)
    {
        return m_service.queueMailboxSelectionMutation(std::move(intent));
    }

    QueuedMessageSelectionMutationResult
    MailCommandService::queueDestroyMessages(std::string accountId,
                                             std::optional<std::string> sourceMailboxId,
                                             MessageSelection selection)
    {
        return m_service.queueDestroyMessages(std::move(accountId), std::move(sourceMailboxId),
                                              std::move(selection));
    }

    QueuedMessageSelectionMutationResult
    MailCommandService::queueMarkMessagesUnread(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                MessageSelection selection)
    {
        return m_service.queueMarkMessagesUnread(std::move(accountId), std::move(sourceMailboxId),
                                                 std::move(selection));
    }

    QueuedMessageSelectionMutationResult
    MailCommandService::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        return m_service.queueMarkEmailRead(std::move(accountId), std::move(emailId));
    }

    QueuedMessageSelectionMutationResult
    MailCommandService::queueSetEmailFlagged(std::string accountId, std::string emailId,
                                             const bool flagged)
    {
        return m_service.queueSetEmailFlagged(std::move(accountId), std::move(emailId), flagged);
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailCommandService::submitPendingEmailMutations(std::string accountId,
                                                    std::optional<std::string> operationGroupId)
    {
        return m_service.submitPendingEmailMutations(std::move(accountId),
                                                     std::move(operationGroupId));
    }

} // namespace javelin::app
