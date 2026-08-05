#pragma once

#include "app/ContactCommands.h"
#include "jmap/OperationError.h"
#include "jmap/api/ContactsMethods.h"

#include <variant>

namespace javelin::app
{
    using PreparedAddressBookSetRequest =
        std::variant<javelin::jmap::api::AddressBookSetRequest, javelin::jmap::OperationError>;
    using PreparedContactSetRequest =
        std::variant<javelin::jmap::api::ContactCardSetRequest, javelin::jmap::OperationError>;
    using PreparedContactCopyRequest =
        std::variant<javelin::jmap::api::ContactCardCopyRequest, javelin::jmap::OperationError>;

    [[nodiscard]] PreparedAddressBookSetRequest
    prepareAddressBookMutation(AddressBookCommand command);
    [[nodiscard]] PreparedContactSetRequest prepareSaveContact(SaveContactCommand command);
    [[nodiscard]] PreparedContactSetRequest
    prepareSetContactsStarred(SetContactsStarredCommand command);
    [[nodiscard]] PreparedContactSetRequest prepareDeleteContacts(DeleteContactsCommand command);
    [[nodiscard]] PreparedContactSetRequest
    prepareDeleteContactGroup(DeleteContactGroupCommand command);
    [[nodiscard]] PreparedContactCopyRequest prepareCopyContact(CopyContactCommand command);
    [[nodiscard]] PreparedContactSetRequest prepareCrossConnectionCopy(CopyContactCommand command);
    [[nodiscard]] PreparedContactSetRequest prepareImportContacts(ImportContactsCommand command);
    [[nodiscard]] PreparedContactSetRequest prepareMergeContacts(MergeContactsCommand command);
} // namespace javelin::app
