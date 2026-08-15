#include "daemon/actions/DaemonRemoteActionDispatcher.h"

#include "app/MailApplicationPorts.h"
#include "app/MailQueryApplicationService.h"
#include "app/MessageContentApplicationPorts.h"
#include "daemon/DaemonServices.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchMailAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::MailQueueMailboxMutation::id.value:
            return dispatchDecoded<actions::MailQueueMailboxMutation>(
                id, command,
                [this, &id](MailboxSelectionMutationIntent intent)
                {
                    return launchAction<actions::MailQueueMailboxMutation>(
                        id, m_services.mailCommandPort().queueMailboxSelectionMutation(
                                std::move(intent)));
                });
        case actions::MailTransferAcrossAccounts::id.value:
            return dispatchDecoded<actions::MailTransferAcrossAccounts>(
                id, command,
                [this, &id](CrossAccountMailTransferIntent intent)
                {
                    return launchAction<actions::MailTransferAcrossAccounts>(
                        id, m_services.mailCommandPort().transferAcrossAccounts(std::move(intent)));
                });
        case actions::MailQueueDestroy::id.value:
            return dispatchDecoded<actions::MailQueueDestroy>(
                id, command,
                [this, &id](std::string accountId, std::optional<std::string> mailboxId,
                            MessageSelection selection)
                {
                    return launchAction<actions::MailQueueDestroy>(
                        id, m_services.mailCommandPort().queueDestroyMessages(
                                std::move(accountId), std::move(mailboxId), std::move(selection)));
                });
        case actions::MailQueueMarkUnread::id.value:
            return dispatchDecoded<actions::MailQueueMarkUnread>(
                id, command,
                [this, &id](std::string accountId, std::optional<std::string> mailboxId,
                            MessageSelection selection)
                {
                    return launchAction<actions::MailQueueMarkUnread>(
                        id, m_services.mailCommandPort().queueMarkMessagesUnread(
                                std::move(accountId), std::move(mailboxId), std::move(selection)));
                });
        case actions::MailQueueMarkRead::id.value:
            return dispatchDecoded<actions::MailQueueMarkRead>(
                id, command,
                [this, &id](std::string accountId, std::string emailId)
                {
                    return launchAction<actions::MailQueueMarkRead>(
                        id, m_services.mailCommandPort().queueMarkEmailRead(std::move(accountId),
                                                                            std::move(emailId)));
                });
        case actions::MailQueueSetFlagged::id.value:
            return dispatchDecoded<actions::MailQueueSetFlagged>(
                id, command,
                [this, &id](std::string accountId, std::string emailId, const bool flagged)
                {
                    MessageSelection selection;
                    selection.emplace_back(SelectedEmail{.emailId = std::move(emailId)});
                    return launchAction<actions::MailQueueSetFlagged>(
                        id, m_services.mailCommandPort().queueSetMessagesFlagged(
                                std::move(accountId), std::nullopt, std::move(selection), flagged));
                });
        case actions::MailQueueSetSelectionFlagged::id.value:
            return dispatchDecoded<actions::MailQueueSetSelectionFlagged>(
                id, command,
                [this, &id](std::string accountId, std::optional<std::string> mailboxId,
                            MessageSelection selection, const bool flagged)
                {
                    return launchAction<actions::MailQueueSetSelectionFlagged>(
                        id, m_services.mailCommandPort().queueSetMessagesFlagged(
                                std::move(accountId), std::move(mailboxId), std::move(selection),
                                flagged));
                });
        case actions::MailQueueSetTag::id.value:
            return dispatchDecoded<actions::MailQueueSetTag>(
                id, command,
                [this, &id](std::string accountId, std::optional<std::string> mailboxId,
                            MessageSelection selection, std::string keyword, const bool enabled)
                {
                    return launchAction<actions::MailQueueSetTag>(
                        id, m_services.mailCommandPort().queueSetMessagesTag(
                                std::move(accountId), std::move(mailboxId), std::move(selection),
                                std::move(keyword), enabled));
                });
        case actions::MailSaveTagDefinition::id.value:
            return dispatchDecoded<actions::MailSaveTagDefinition>(
                id, command,
                [this, &id](SaveMailTagDefinition definition)
                {
                    return launchAction<actions::MailSaveTagDefinition>(
                        id, m_services.mailCommandPort().saveTagDefinition(std::move(definition)));
                });
        case actions::MailDeleteTag::id.value:
            return dispatchDecoded<actions::MailDeleteTag>(
                id, command,
                [this, &id](std::string accountId, std::string keyword)
                {
                    return launchAction<actions::MailDeleteTag>(
                        id, m_services.mailCommandPort().deleteTag(std::move(accountId),
                                                                   std::move(keyword)));
                });
        case actions::MailSetMailboxSubscribed::id.value:
            return dispatchDecoded<actions::MailSetMailboxSubscribed>(
                id, command,
                [this, &id](std::string accountId, std::string mailboxId, const bool subscribed)
                {
                    return launchAction<actions::MailSetMailboxSubscribed>(
                        id, m_services.mailCommandPort().setMailboxSubscribed(
                                std::move(accountId), std::move(mailboxId), subscribed));
                });
        case actions::MailCreateMailbox::id.value:
            return dispatchDecoded<actions::MailCreateMailbox>(
                id, command,
                [this, &id](std::string accountId, std::string name)
                {
                    return launchAction<actions::MailCreateMailbox>(
                        id, m_services.mailCommandPort().createMailbox(std::move(accountId),
                                                                       std::move(name)));
                });
        case actions::MailDestroyMailbox::id.value:
            return dispatchDecoded<actions::MailDestroyMailbox>(
                id, command,
                [this, &id](std::string accountId, std::string mailboxId)
                {
                    return launchAction<actions::MailDestroyMailbox>(
                        id, m_services.mailCommandPort().destroyMailbox(std::move(accountId),
                                                                        std::move(mailboxId)));
                });
        case actions::MailSubmitPending::id.value:
            return dispatchDecoded<actions::MailSubmitPending>(
                id, command,
                [this, &id](std::string accountId, std::optional<std::string> operationGroupId)
                {
                    return launchAction<actions::MailSubmitPending>(
                        id, m_services.mailCommandPort().submitPendingEmailMutations(
                                std::move(accountId), std::move(operationGroupId)));
                });
        case actions::MessageContent::id.value:
            return dispatchDecoded<actions::MessageContent>(
                id, command,
                [this, &id](std::string accountId, std::string emailId)
                {
                    return launchAction<actions::MessageContent>(
                        id, m_services.messageContentPort().requestMessageContent(
                                std::move(accountId), std::move(emailId)));
                });
        case actions::AttachmentDownload::id.value:
            return dispatchDecoded<actions::AttachmentDownload>(
                id, command,
                [this, &id](std::string accountId, std::string emailId, std::string partId)
                {
                    return launchAction<actions::AttachmentDownload>(
                        id, m_services.messageContentPort().requestAttachment(
                                std::move(accountId), std::move(emailId), std::move(partId)));
                });
        case actions::MessageSource::id.value:
            return dispatchDecoded<actions::MessageSource>(
                id, command,
                [this, &id](std::string accountId, std::string emailId)
                {
                    return launchAction<actions::MessageSource>(
                        id, m_services.messageContentPort().requestMessageSource(
                                std::move(accountId), std::move(emailId)));
                });
        case actions::MailboxObserve::id.value:
            return dispatchDecoded<actions::MailboxObserve>(
                id, command,
                [this, &id](QString observationId, std::string accountId, std::string mailboxId)
                {
                    if (observationId.isEmpty() || m_mailboxObservations.contains(observationId))
                        return reject(
                            id, QStringLiteral("The mailbox observation identifier is invalid."));
                    m_mailboxObservations.emplace(
                        observationId, std::make_unique<MailboxObservation>(
                                           m_services.mailQueryApplicationService().observeMailbox(
                                               std::move(accountId), std::move(mailboxId))));
                    return acceptEmpty<actions::MailboxObserve>(id);
                });
        case actions::MailboxUnobserve::id.value:
            return dispatchDecoded<actions::MailboxUnobserve>(
                id, command,
                [this, &id](const QString& observationId)
                {
                    m_mailboxObservations.erase(observationId);
                    return acceptEmpty<actions::MailboxUnobserve>(id);
                });
        case actions::MailboxWindow::id.value:
            return dispatchDecoded<actions::MailboxWindow>(
                id, command,
                [this, &id](MailboxWindowIntent intent)
                {
                    return launchAction<actions::MailboxWindow>(
                        id, m_services.mailQueryApplicationService().requestMailboxWindow(
                                std::move(intent)));
                });
        case actions::SearchWindow::id.value:
            return dispatchDecoded<actions::SearchWindow>(
                id, command,
                [this, &id](SearchWindowIntent intent)
                {
                    return launchAction<actions::SearchWindow>(
                        id, m_services.mailQueryApplicationService().requestSearchWindow(
                                std::move(intent)));
                });
        case actions::SearchRetire::id.value:
            return dispatchDecoded<actions::SearchRetire>(
                id, command,
                [this, &id](std::string accountId, std::string windowKey)
                {
                    m_services.mailQueryApplicationService().retireSearchWindow(
                        std::move(accountId), std::move(windowKey));
                    return acceptEmpty<actions::SearchRetire>(id);
                });
        case actions::ThreadEnsure::id.value:
            return dispatchDecoded<actions::ThreadEnsure>(
                id, command,
                [this, &id](ThreadMaterializationIntent intent)
                {
                    m_services.mailQueryApplicationService().ensureThread(std::move(intent));
                    return acceptEmpty<actions::ThreadEnsure>(id);
                });
        default:
            return reject(id, QStringLiteral("The mail action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
