#include "app/MailCommandService.h"

#include "app/MailMutationApplicationService.h"
#include "app/MailTransferCommandService.h"

#include <utility>

namespace javelin::app
{
    MailCommandService::MailCommandService(MailMutationApplicationService& service,
                                           MailTransferCommandService& transferService)
        : m_service(service), m_transferService(transferService)
    {
    }

    QCoro::Task<QueuedMailboxSelectionMutationResult>
    MailCommandService::queueMailboxSelectionMutation(MailboxSelectionMutationIntent intent)
    {
        co_return co_await m_service.queueMailboxSelectionMutation(std::move(intent));
    }

    QCoro::Task<MailTransferExecutionResult>
    MailCommandService::transferAcrossAccounts(CrossAccountMailTransferIntent intent)
    {
        co_return co_await m_transferService.transfer(std::move(intent));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueDestroyMessages(std::string accountId,
                                             std::optional<std::string> sourceMailboxId,
                                             MessageSelection selection)
    {
        co_return co_await m_service.queueDestroyMessages(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueMarkMessagesUnread(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                MessageSelection selection)
    {
        co_return co_await m_service.queueMarkMessagesUnread(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueMarkEmailRead(std::string accountId, std::string emailId)
    {
        co_return m_service.queueMarkEmailRead(std::move(accountId), std::move(emailId));
    }

    QCoro::Task<QueuedMessageSelectionMutationResult>
    MailCommandService::queueSetMessagesFlagged(std::string accountId,
                                                std::optional<std::string> sourceMailboxId,
                                                MessageSelection selection, const bool flagged)
    {
        co_return co_await m_service.queueSetMessagesFlagged(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection), flagged);
    }

    QCoro::Task<QueuedMessageSelectionMutationResult> MailCommandService::queueSetMessagesTag(
        std::string accountId, std::optional<std::string> sourceMailboxId,
        MessageSelection selection, std::string keyword, const bool enabled)
    {
        co_return co_await m_service.queueSetMessagesTag(
            std::move(accountId), std::move(sourceMailboxId), std::move(selection),
            std::move(keyword), enabled);
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

    QCoro::Task<javelin::jmap::MailboxSubscriptionChangeResult>
    MailCommandService::setMailboxSubscribed(std::string accountId, std::string mailboxId,
                                             const bool subscribed)
    {
        co_return co_await m_service.setMailboxSubscribed(std::move(accountId),
                                                          std::move(mailboxId), subscribed);
    }

    QCoro::Task<javelin::jmap::MailboxCreateResult>
    MailCommandService::createMailbox(std::string accountId, std::string name)
    {
        co_return co_await m_service.createMailbox(std::move(accountId), std::move(name));
    }

    QCoro::Task<javelin::jmap::MailboxDestroyResult>
    MailCommandService::destroyMailbox(std::string accountId, std::string mailboxId)
    {
        co_return co_await m_service.destroyMailbox(std::move(accountId), std::move(mailboxId));
    }

    QCoro::Task<javelin::jmap::SubmittedEmailMutationsResult>
    MailCommandService::submitPendingEmailMutations(std::string accountId,
                                                    std::optional<std::string> operationGroupId)
    {
        return m_service.submitPendingEmailMutations(std::move(accountId),
                                                     std::move(operationGroupId));
    }

} // namespace javelin::app
