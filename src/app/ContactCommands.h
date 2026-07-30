#pragma once

#include "jmap/contacts/ContactTypes.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace javelin::app
{
    struct CreateAddressBookCommand
    {
        std::string accountId;
        javelin::jmap::api::AddressBook addressBook;
    };

    struct UpdateAddressBookCommand
    {
        std::string accountId;
        javelin::jmap::api::AddressBook addressBook;
    };

    struct DeleteAddressBookCommand
    {
        std::string accountId;
        std::string addressBookId;
        bool removeContents = true;
    };

    struct SetDefaultAddressBookCommand
    {
        std::string accountId;
        std::string addressBookId;
    };

    using AddressBookCommand = std::variant<CreateAddressBookCommand, UpdateAddressBookCommand,
                                            DeleteAddressBookCommand, SetDefaultAddressBookCommand>;

    struct ContactDocumentSnapshot
    {
        std::string id;
        std::string document;
    };

    struct CreateContactGroupCommand
    {
        std::string accountId;
        std::string addressBookId;
        std::string name;
    };

    struct DeleteContactGroupCommand
    {
        std::string accountId;
        std::string groupId;
    };

    struct SetContactGroupMembershipCommand
    {
        std::string accountId;
        std::string groupId;
        std::vector<std::string> memberUids;
        bool included = true;
    };

    struct SaveContactCommand
    {
        std::string accountId;
        std::optional<std::string> contactId;
        javelin::jmap::contacts::ContactEditorData contact;
    };

    struct SetContactsStarredCommand
    {
        std::string accountId;
        std::vector<ContactDocumentSnapshot> contacts;
        bool starred = true;
    };

    struct DeleteContactsCommand
    {
        std::string accountId;
        std::vector<std::string> contactIds;
    };

    struct CopyContactCommand
    {
        std::string sourceAccountId;
        std::string destinationAccountId;
        std::string contactId;
        std::string destinationAddressBookId;
    };

    struct ImportContactsCommand
    {
        std::string accountId;
        std::string addressBookId;
        std::vector<javelin::jmap::contacts::ContactEditorData> contacts;
        std::unordered_set<std::string> knownUids;
    };

    struct MergeContactsCommand
    {
        std::string accountId;
        ContactDocumentSnapshot primary;
        std::vector<ContactDocumentSnapshot> duplicates;
    };
} // namespace javelin::app
