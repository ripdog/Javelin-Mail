#pragma once

#include "app/ContactApplicationPorts.h"

#include <QByteArray>
#include <QString>

#include <string>

namespace javelin::jmap::contacts
{
    class ContactService;
}

namespace javelin::app
{
    class AccountConnectionProvider;
    class ApplicationErrorCoordinator;
    class WorkScheduler;

    class ContactCommandService final : public ContactCommandPort
    {
      public:
        ContactCommandService(AccountConnectionProvider& connectionProvider,
                              javelin::jmap::contacts::ContactService& contactService,
                              ApplicationErrorCoordinator& errorCoordinator,
                              WorkScheduler& workScheduler);

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

      private:
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        submitAddressBooks(std::string ownerAccountId,
                           javelin::jmap::api::AddressBookSetRequest request);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        submitContactCards(std::string ownerAccountId,
                           javelin::jmap::api::ContactCardSetRequest request,
                           QString operationDescription);
        [[nodiscard]] QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        submitContactCopy(std::string ownerAccountId,
                          javelin::jmap::api::ContactCardCopyRequest request,
                          QString operationDescription);

        AccountConnectionProvider& m_connectionProvider;
        javelin::jmap::contacts::ContactService& m_contactService;
        ApplicationErrorCoordinator& m_errorCoordinator;
        WorkScheduler& m_workScheduler;
    };
} // namespace javelin::app
