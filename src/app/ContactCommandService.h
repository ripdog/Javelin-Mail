#pragma once

#include "app/ContactApplicationPorts.h"
#include "app/undo/AddressBookHistoryPort.h"
#include "app/undo/ContactHistoryPort.h"

#include <QByteArray>
#include <QString>

#include <string>

namespace javelin::jmap::contacts
{
    class ContactMediaService;
    class ContactMutationEngine;
    class ContactSyncEngine;
} // namespace javelin::jmap::contacts

namespace javelin::jmap::cache
{
    class ContactRepository;
}

namespace javelin::app
{
    class AccountConnectionProvider;
    class ApplicationErrorCoordinator;
    class WorkScheduler;
    namespace undo
    {
        class UndoManager;
    }

    class ContactCommandService final : public ContactCommandPort,
                                        public undo::ContactHistoryPort,
                                        public undo::AddressBookHistoryPort
    {
      public:
        ContactCommandService(AccountConnectionProvider& connectionProvider,
                              javelin::jmap::contacts::ContactSyncEngine& syncEngine,
                              javelin::jmap::contacts::ContactMutationEngine& mutationEngine,
                              javelin::jmap::contacts::ContactMediaService& mediaService,
                              javelin::jmap::cache::ContactRepository& contactRepository,
                              ApplicationErrorCoordinator& errorCoordinator,
                              WorkScheduler& workScheduler, undo::UndoManager& undoManager);

        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mutateAddressBook(std::string ownerAccountId, AddressBookCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        saveContact(std::string ownerAccountId, SaveContactCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactsStarred(std::string ownerAccountId, SetContactsStarredCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContacts(std::string ownerAccountId, DeleteContactsCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        createContactGroup(std::string ownerAccountId, CreateContactGroupCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContactGroup(std::string ownerAccountId, DeleteContactGroupCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactGroupMembership(std::string ownerAccountId,
                                  SetContactGroupMembershipCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        copyContact(std::string ownerAccountId, CopyContactCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        importContacts(std::string ownerAccountId, ImportContactsCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mergeContacts(std::string ownerAccountId, MergeContactsCommand command) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
        uploadContactMedia(std::string ownerAccountId, std::string accountId, QByteArray payload,
                           std::string mediaType) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
        downloadContactMedia(std::string ownerAccountId, std::string accountId, std::string blobId,
                             std::string mediaType) override;
        [[nodiscard]] QCoro::Task<undo::AuthoritativeContactsResult>
        getAuthoritativeContacts(std::string ownerAccountId, std::string accountId) override;
        [[nodiscard]] undo::AuthoritativeContactsResult
        getEffectiveContacts(std::string_view accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        applyContactCardsFromHistory(std::string ownerAccountId,
                                     javelin::jmap::api::ContactCardSetRequest request,
                                     undo::CommandOrigin origin) override;
        [[nodiscard]] QCoro::Task<undo::AuthoritativeAddressBooksResult>
        getAuthoritativeAddressBooks(std::string ownerAccountId, std::string accountId) override;
        [[nodiscard]] undo::AuthoritativeAddressBooksResult
        getEffectiveAddressBooks(std::string_view accountId) override;
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        applyAddressBooksFromHistory(std::string ownerAccountId,
                                     javelin::jmap::api::AddressBookSetRequest request,
                                     undo::CommandOrigin origin) override;

      private:
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        submitAddressBooks(std::string ownerAccountId,
                           javelin::jmap::api::AddressBookSetRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        submitContactCards(std::string ownerAccountId,
                           javelin::jmap::api::ContactCardSetRequest request,
                           QString operationDescription, QString actionDescription = {});
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        submitContactCopy(std::string ownerAccountId,
                          javelin::jmap::api::ContactCardCopyRequest request,
                          QString operationDescription);

        AccountConnectionProvider& m_connectionProvider;
        javelin::jmap::contacts::ContactSyncEngine& m_contactSyncEngine;
        javelin::jmap::contacts::ContactMutationEngine& m_contactMutationEngine;
        javelin::jmap::contacts::ContactMediaService& m_contactMediaService;
        javelin::jmap::cache::ContactRepository& m_contactRepository;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
        undo::UndoManager& m_undoManager;
    };
} // namespace javelin::app
