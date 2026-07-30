#pragma once

#include "app/ContactCommands.h"
#include "jmap/contacts/ContactResults.h"

#include <QCoroTask>

#include <QByteArray>

#include <string>

namespace javelin::app
{
    class ContactRefreshPort
    {
      public:
        virtual ~ContactRefreshPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactRefreshResult>
        requestContacts(std::string ownerAccountId) = 0;
    };

    class ContactCommandPort
    {
      public:
        virtual ~ContactCommandPort() = default;

        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mutateAddressBook(std::string ownerAccountId, AddressBookCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        saveContact(std::string ownerAccountId, SaveContactCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactsStarred(std::string ownerAccountId, SetContactsStarredCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContacts(std::string ownerAccountId, DeleteContactsCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        createContactGroup(std::string ownerAccountId, CreateContactGroupCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        deleteContactGroup(std::string ownerAccountId, DeleteContactGroupCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        setContactGroupMembership(std::string ownerAccountId,
                                  SetContactGroupMembershipCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        copyContact(std::string ownerAccountId, CopyContactCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        importContacts(std::string ownerAccountId, ImportContactsCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactMutationResult>
        mergeContacts(std::string ownerAccountId, MergeContactsCommand command) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactUploadResult>
        uploadContactMedia(std::string ownerAccountId, std::string accountId, QByteArray payload,
                           std::string mediaType) = 0;
        [[nodiscard]] virtual QCoro::Task<javelin::jmap::contacts::ContactDownloadResult>
        downloadContactMedia(std::string ownerAccountId, std::string accountId, std::string blobId,
                             std::string mediaType) = 0;
    };
} // namespace javelin::app
