#include "daemon/actions/DaemonRemoteActionDispatcher.h"

#include "app/AccountRefreshApplicationPorts.h"
#include "app/ContactApplicationPorts.h"
#include "daemon/DaemonServices.h"

namespace javelin::app
{
    javelin::protocol::CommandReply DaemonRemoteActionDispatcher::dispatchContactAction(
        const javelin::protocol::CommandId& id,
        const javelin::protocol::RemoteActionCommand& command)
    {
        namespace actions = javelin::protocol::actions;
        switch (command.action.value)
        {
        case actions::ContactRequestRefresh::id.value:
            return dispatchDecoded<actions::ContactRequestRefresh>(
                id, command,
                [this, &id](std::string ownerAccountId)
                {
                    return launchAction<actions::ContactRequestRefresh>(
                        id,
                        m_services.accountRefreshPort().requestContacts(std::move(ownerAccountId)));
                });
        case actions::ContactMutateAddressBook::id.value:
            return dispatchDecoded<actions::ContactMutateAddressBook>(
                id, command,
                [this, &id](std::string ownerAccountId, AddressBookCommand action)
                {
                    return launchAction<actions::ContactMutateAddressBook>(
                        id, m_services.contactCommandPort().mutateAddressBook(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactSave::id.value:
            return dispatchDecoded<actions::ContactSave>(
                id, command,
                [this, &id](std::string ownerAccountId, SaveContactCommand action)
                {
                    return launchAction<actions::ContactSave>(
                        id, m_services.contactCommandPort().saveContact(std::move(ownerAccountId),
                                                                        std::move(action)));
                });
        case actions::ContactSetStarred::id.value:
            return dispatchDecoded<actions::ContactSetStarred>(
                id, command,
                [this, &id](std::string ownerAccountId, SetContactsStarredCommand action)
                {
                    return launchAction<actions::ContactSetStarred>(
                        id, m_services.contactCommandPort().setContactsStarred(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactDelete::id.value:
            return dispatchDecoded<actions::ContactDelete>(
                id, command,
                [this, &id](std::string ownerAccountId, DeleteContactsCommand action)
                {
                    return launchAction<actions::ContactDelete>(
                        id, m_services.contactCommandPort().deleteContacts(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactCreateGroup::id.value:
            return dispatchDecoded<actions::ContactCreateGroup>(
                id, command,
                [this, &id](std::string ownerAccountId, CreateContactGroupCommand action)
                {
                    return launchAction<actions::ContactCreateGroup>(
                        id, m_services.contactCommandPort().createContactGroup(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactDeleteGroup::id.value:
            return dispatchDecoded<actions::ContactDeleteGroup>(
                id, command,
                [this, &id](std::string ownerAccountId, DeleteContactGroupCommand action)
                {
                    return launchAction<actions::ContactDeleteGroup>(
                        id, m_services.contactCommandPort().deleteContactGroup(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactSetGroupMembership::id.value:
            return dispatchDecoded<actions::ContactSetGroupMembership>(
                id, command,
                [this, &id](std::string ownerAccountId, SetContactGroupMembershipCommand action)
                {
                    return launchAction<actions::ContactSetGroupMembership>(
                        id, m_services.contactCommandPort().setContactGroupMembership(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactCopy::id.value:
            return dispatchDecoded<actions::ContactCopy>(
                id, command,
                [this, &id](std::string ownerAccountId, CopyContactCommand action)
                {
                    return launchAction<actions::ContactCopy>(
                        id, m_services.contactCommandPort().copyContact(std::move(ownerAccountId),
                                                                        std::move(action)));
                });
        case actions::ContactImport::id.value:
            return dispatchDecoded<actions::ContactImport>(
                id, command,
                [this, &id](std::string ownerAccountId, ImportContactsCommand action)
                {
                    return launchAction<actions::ContactImport>(
                        id, m_services.contactCommandPort().importContacts(
                                std::move(ownerAccountId), std::move(action)));
                });
        case actions::ContactMerge::id.value:
            return dispatchDecoded<actions::ContactMerge>(
                id, command,
                [this, &id](std::string ownerAccountId, MergeContactsCommand action)
                {
                    return launchAction<actions::ContactMerge>(
                        id, m_services.contactCommandPort().mergeContacts(std::move(ownerAccountId),
                                                                          std::move(action)));
                });
        case actions::ContactUploadMedia::id.value:
            return dispatchDecoded<actions::ContactUploadMedia>(
                id, command,
                [this, &id](std::string ownerAccountId, std::string accountId, QByteArray payload,
                            std::string mediaType)
                {
                    return launchAction<actions::ContactUploadMedia>(
                        id, m_services.contactCommandPort().uploadContactMedia(
                                std::move(ownerAccountId), std::move(accountId), std::move(payload),
                                std::move(mediaType)));
                });
        case actions::ContactDownloadMedia::id.value:
            return dispatchDecoded<actions::ContactDownloadMedia>(
                id, command,
                [this, &id](std::string ownerAccountId, std::string accountId, std::string blobId,
                            std::string mediaType)
                {
                    return launchAction<actions::ContactDownloadMedia>(
                        id, m_services.contactCommandPort().downloadContactMedia(
                                std::move(ownerAccountId), std::move(accountId), std::move(blobId),
                                std::move(mediaType)));
                });
        default:
            return reject(id, QStringLiteral("The contact action is unsupported."),
                          javelin::protocol::BoundaryErrorCode::UnsupportedOperation);
        }
    }
} // namespace javelin::app
