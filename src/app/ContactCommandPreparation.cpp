#include "app/ContactCommandPreparation.h"

#include "jmap/contacts/ContactInterchange.h"

#include <QUuid>

#include <string_view>
#include <type_traits>
#include <utility>

namespace javelin::app
{
    namespace
    {
        [[nodiscard]] javelin::jmap::OperationError
        invalidContactCommand(const std::string_view message)
        {
            return javelin::jmap::OperationError{
                .code = javelin::jmap::OperationErrorCode::InvalidUserInput,
                .message =
                    QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())),
            };
        }

        [[nodiscard]] std::string generatedUid()
        {
            return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        }
    } // namespace

    PreparedAddressBookSetRequest prepareAddressBookMutation(AddressBookCommand command)
    {
        return std::visit(
            [](auto concreteCommand) -> PreparedAddressBookSetRequest
            {
                using Command = std::decay_t<decltype(concreteCommand)>;
                if (concreteCommand.accountId.empty())
                    return invalidContactCommand("Changing an address book requires an account.");

                javelin::jmap::api::AddressBookSetRequest request;
                request.accountId = std::move(concreteCommand.accountId);
                if constexpr (std::is_same_v<Command, CreateAddressBookCommand>)
                {
                    request.create.emplace(
                        "new-address-book",
                        javelin::jmap::api::addressBookCreateDocument(concreteCommand.addressBook));
                }
                else if constexpr (std::is_same_v<Command, UpdateAddressBookCommand>)
                {
                    if (concreteCommand.addressBook.id.empty())
                        return invalidContactCommand(
                            "Updating an address book requires an address book id.");
                    request.update.emplace(
                        concreteCommand.addressBook.id,
                        javelin::jmap::api::addressBookUpdateDocument(concreteCommand.addressBook));
                }
                else if constexpr (std::is_same_v<Command, DeleteAddressBookCommand>)
                {
                    if (concreteCommand.addressBookId.empty())
                        return invalidContactCommand(
                            "Deleting an address book requires an address book id.");
                    request.destroy.push_back(std::move(concreteCommand.addressBookId));
                    request.onDestroyRemoveContents = concreteCommand.removeContents;
                }
                else
                {
                    if (concreteCommand.addressBookId.empty())
                        return invalidContactCommand(
                            "Selecting a default address book requires an address book id.");
                    request.onSuccessSetIsDefault = std::move(concreteCommand.addressBookId);
                }
                return request;
            },
            std::move(command));
    }

    PreparedContactSetRequest prepareSaveContact(SaveContactCommand command)
    {
        if (command.accountId.empty())
            return invalidContactCommand("Saving a contact requires an account.");

        const bool creating = !command.contactId.has_value();
        const auto document =
            javelin::jmap::contacts::applyContactEditorData(command.contact, creating);
        if (const auto* message = std::get_if<std::string_view>(&document))
            return invalidContactCommand(*message);

        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = std::move(command.accountId);
        if (creating)
        {
            request.create.emplace("new-contact", javelin::jmap::api::ContactDocument{
                                                      .json = std::get<std::string>(document)});
        }
        else if (command.contactId->empty())
        {
            return invalidContactCommand("Updating a contact requires a contact id.");
        }
        else
        {
            request.update.emplace(
                std::move(*command.contactId),
                javelin::jmap::api::ContactDocument{.json = std::get<std::string>(document)});
        }
        return request;
    }

    PreparedContactSetRequest prepareSetContactsStarred(SetContactsStarredCommand command)
    {
        if (command.accountId.empty() || command.contacts.empty())
            return invalidContactCommand("Starring contacts requires an account and contacts.");

        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = std::move(command.accountId);
        for (auto& contact : command.contacts)
        {
            if (contact.id.empty())
                return invalidContactCommand("Starring a contact requires a contact id.");
            const auto document =
                javelin::jmap::contacts::setContactStarred(contact.document, command.starred);
            if (const auto* message = std::get_if<std::string_view>(&document))
                return invalidContactCommand(*message);
            request.update.emplace(
                std::move(contact.id),
                javelin::jmap::api::ContactDocument{.json = std::get<std::string>(document)});
        }
        return request;
    }

    PreparedContactSetRequest prepareDeleteContacts(DeleteContactsCommand command)
    {
        if (command.accountId.empty() || command.contactIds.empty())
            return invalidContactCommand("Deleting contacts requires an account and contact ids.");
        for (const auto& id : command.contactIds)
            if (id.empty())
                return invalidContactCommand("Deleting a contact requires a contact id.");

        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = std::move(command.accountId);
        request.destroy = std::move(command.contactIds);
        return request;
    }

    PreparedContactCopyRequest prepareCopyContact(CopyContactCommand command)
    {
        if (command.sourceAccountId.empty() || command.destinationAccountId.empty())
            return invalidContactCommand(
                "Copying a contact requires source and destination accounts.");

        const auto document = javelin::jmap::contacts::copyContactDocument(
            std::move(command.contactId), std::move(command.destinationAddressBookId));
        if (const auto* message = std::get_if<std::string_view>(&document))
            return invalidContactCommand(*message);

        javelin::jmap::api::ContactCardCopyRequest request;
        request.fromAccountId = std::move(command.sourceAccountId);
        request.accountId = std::move(command.destinationAccountId);
        request.create.emplace("copy-contact", javelin::jmap::api::ContactDocument{
                                                   .json = std::get<std::string>(document)});
        return request;
    }

    PreparedContactSetRequest prepareImportContacts(ImportContactsCommand command)
    {
        if (command.accountId.empty() || command.addressBookId.empty() || command.contacts.empty())
            return invalidContactCommand(
                "Importing contacts requires an account, address book, and contacts.");

        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = std::move(command.accountId);
        for (std::size_t index = 0; index < command.contacts.size(); ++index)
        {
            auto& contact = command.contacts[index];
            if (!contact.uid.empty() && !command.knownUids.insert(contact.uid).second)
            {
                return invalidContactCommand(
                    "The imported vCard contains a UID already present in this account.");
            }
            const auto document = javelin::jmap::contacts::importedContactDocument(
                std::move(contact), command.addressBookId, generatedUid());
            if (const auto* message = std::get_if<std::string_view>(&document))
                return invalidContactCommand(*message);
            request.create.emplace(
                "import-" + std::to_string(index + 1),
                javelin::jmap::api::ContactDocument{.json = std::get<std::string>(document)});
        }
        return request;
    }

    PreparedContactSetRequest prepareMergeContacts(MergeContactsCommand command)
    {
        if (command.accountId.empty() || command.primary.id.empty() || command.duplicates.empty())
            return invalidContactCommand(
                "Merging contacts requires an account, a primary contact, and duplicates.");

        std::string mergedDocument = std::move(command.primary.document);
        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = std::move(command.accountId);
        for (const auto& duplicate : command.duplicates)
        {
            if (duplicate.id.empty() || duplicate.id == command.primary.id)
                return invalidContactCommand(
                    "Each merged duplicate requires a distinct contact id.");
            const auto merged =
                javelin::jmap::contacts::mergeContactDocuments(mergedDocument, duplicate.document);
            if (const auto* message = std::get_if<std::string_view>(&merged))
                return invalidContactCommand(*message);
            mergedDocument = std::get<std::string>(merged);
            request.destroy.push_back(duplicate.id);
        }
        request.update.emplace(
            std::move(command.primary.id),
            javelin::jmap::api::ContactDocument{.json = std::move(mergedDocument)});
        return request;
    }
} // namespace javelin::app
