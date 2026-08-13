#include "tools/UndoRedoAutonomousSuite.h"

#include "app/CalendarApplicationService.h"
#include "app/ComposeService.h"
#include "app/ContactApplicationPorts.h"
#include "app/undo/AddressBookHistoryExecutor.h"
#include "app/undo/AddressBookHistoryPort.h"
#include "app/undo/CalendarHistoryExecutor.h"
#include "app/undo/CalendarHistoryPort.h"
#include "app/undo/ContactHistoryExecutor.h"
#include "app/undo/ContactHistoryPort.h"
#include "app/undo/DraftHistoryExecutor.h"
#include "daemon/DaemonServices.h"
#include "jmap/api/CalendarMethods.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/contacts/ContactTypes.h"
#include "jmap/submission/DraftSnapshotSerialization.h"
#include "jmap/sync/MutationJournal.h"

#include <QUuid>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace javelin::tools
{
    namespace
    {
        using javelin::app::undo::AddressBookHistory;
        using javelin::app::undo::AddressBookHistoryExecutor;
        using javelin::app::undo::AddressBookHistoryPort;
        using javelin::app::undo::AuthoritativeAddressBooks;
        using javelin::app::undo::AuthoritativeContacts;
        using javelin::app::undo::CalendarEventHistory;
        using javelin::app::undo::CalendarHistoryExecutor;
        using javelin::app::undo::CommandOrigin;
        using javelin::app::undo::ContactCardHistory;
        using javelin::app::undo::ContactCardItemHistory;
        using javelin::app::undo::ContactHistoryExecutor;
        using javelin::app::undo::ContactHistoryPort;
        using javelin::app::undo::DraftHistory;
        using javelin::app::undo::DraftHistoryExecutor;
        using javelin::app::undo::HistoryEntry;
        using javelin::app::undo::HistoryExecutionDirection;
        using javelin::app::undo::HistoryExecutionOutcome;

        struct ContactFixtureContext
        {
            std::string connectionId;
            std::string accountId;
            std::string addressBookId;
            std::string runTag;
            ContactHistoryPort& port;
            ContactHistoryExecutor executor;
            javelin::jmap::sync::MutationJournalRepository journal;
            std::vector<std::string> fixtureUids;
        };

        struct AddressBookFixtureContext
        {
            std::string connectionId;
            std::string accountId;
            std::string runTag;
            AddressBookHistoryPort& books;
            ContactHistoryPort& contacts;
            AddressBookHistoryExecutor executor;
            javelin::jmap::sync::MutationJournalRepository journal;
        };

        struct CalendarFixtureContext
        {
            std::string connectionId;
            std::string accountId;
            std::string calendarId;
            std::string uid;
            std::string runTag;
            javelin::app::undo::CalendarHistoryPort& port;
            CalendarHistoryExecutor executor;
            javelin::jmap::sync::MutationJournalRepository journal;
        };

        [[nodiscard]] std::runtime_error
        operationFailure(const std::string_view operation,
                         const javelin::jmap::OperationError& error)
        {
            return std::runtime_error(std::string{operation} + ": " + error.message.toStdString());
        }

        [[nodiscard]] QCoro::Task<AuthoritativeContacts> contacts(ContactFixtureContext& context)
        {
            auto loaded = co_await context.port.getAuthoritativeContacts(context.connectionId,
                                                                         context.accountId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                throw operationFailure("refresh contacts", *error);
            co_return std::get<AuthoritativeContacts>(std::move(loaded));
        }

        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::contacts::ContactSummary>>
        findContact(ContactFixtureContext& context, const std::string_view uid)
        {
            auto authoritative = co_await contacts(context);
            const auto found = std::ranges::find(authoritative.contacts, uid,
                                                 &javelin::jmap::contacts::ContactSummary::uid);
            if (found == authoritative.contacts.end())
                co_return std::nullopt;
            co_return *found;
        }

        [[nodiscard]] HistoryEntry contactEntry(const ContactFixtureContext& context,
                                                ContactCardItemHistory item)
        {
            HistoryEntry entry;
            entry.commandKind = QStringLiteral("contact_card");
            entry.payload = ContactCardHistory{
                .connectionId = context.connectionId,
                .accountId = context.accountId,
                .items = {std::move(item)},
            };
            return entry;
        }

        [[nodiscard]] QCoro::Task<HistoryEntry> execute(ContactFixtureContext& context,
                                                        HistoryEntry entry,
                                                        const HistoryExecutionDirection direction,
                                                        const std::string_view step)
        {
            auto result = co_await context.executor.execute(entry, direction);
            if (result.outcome != HistoryExecutionOutcome::Success ||
                !result.updatedPayload.has_value())
            {
                throw std::runtime_error(std::string{step} + ": " + result.summary.toStdString());
            }
            entry.payload = std::move(*result.updatedPayload);
            const auto active = context.journal.listActive({
                .accountId = context.accountId,
                .dataType = "ContactCard",
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
                throw std::runtime_error(error->message.toStdString());
            const auto& records =
                std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active);
            if (std::ranges::any_of(records,
                                    [&context](const auto& record)
                                    {
                                        return std::ranges::any_of(
                                            context.fixtureUids, [&record](const auto& uid)
                                            { return record.payloadJson.contains(uid); });
                                    }))
                throw std::runtime_error(std::string{step} +
                                         ": optimistic contact journal did not drain");
            co_return entry;
        }

        [[nodiscard]] std::string contactDocument(const ContactFixtureContext& context,
                                                  const std::string& uid, const std::string& name)
        {
            javelin::jmap::contacts::ContactEditorData data{
                .uid = uid,
                .kind = "individual",
                .fullName = name,
                .organization = {},
                .title = {},
                .emails = {},
                .phones = {},
                .addresses = {},
                .members = {},
                .birthday = {},
                .notes = {},
                .addressBookIds = {context.addressBookId},
                .document = "{}",
            };
            auto document = javelin::jmap::contacts::applyContactEditorData(data, true);
            if (const auto* error = std::get_if<std::string_view>(&document))
                throw std::runtime_error(std::string{*error});
            return std::get<std::string>(std::move(document));
        }

        [[nodiscard]] QCoro::Task<HistoryEntry> createRoundTrip(ContactFixtureContext& context,
                                                                const std::string& uid,
                                                                const std::string& document,
                                                                const std::string_view label)
        {
            auto entry = contactEntry(context, {
                                                   .addressBookId = context.addressBookId,
                                                   .currentCardId = std::nullopt,
                                                   .uid = uid,
                                                   .beforeDocumentJson = std::nullopt,
                                                   .afterDocumentJson = document,
                                               });
            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                     std::string{label} + " create");
            if (!(co_await findContact(context, uid)).has_value())
                throw std::runtime_error(std::string{label} + " create was not visible");

            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Undo,
                                     std::string{label} + " create undo");
            if ((co_await findContact(context, uid)).has_value())
                throw std::runtime_error(std::string{label} + " remained after create undo");

            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                     std::string{label} + " create redo");
            if (!(co_await findContact(context, uid)).has_value())
                throw std::runtime_error(std::string{label} + " was absent after create redo");
            co_return entry;
        }

        [[nodiscard]] QCoro::Task<void> editRoundTrip(ContactFixtureContext& context,
                                                      const std::string& uid,
                                                      const bool groupMembership,
                                                      const std::optional<std::string>& memberUid)
        {
            const auto before = co_await findContact(context, uid);
            if (!before.has_value())
                throw std::runtime_error("contact edit fixture disappeared");
            auto editor = javelin::jmap::contacts::contactEditorData(before->document);
            if (const auto* error = std::get_if<std::string_view>(&editor))
                throw std::runtime_error(std::string{*error});
            auto data = std::get<javelin::jmap::contacts::ContactEditorData>(std::move(editor));
            if (groupMembership)
            {
                if (!memberUid.has_value())
                    throw std::runtime_error("group membership fixture has no member uid");
                data.members = {*memberUid};
            }
            else
            {
                data.fullName = context.runTag + " edited contact";
                data.organization = "Javelin Undo Lab";
                data.title = "Round-trip tester";
                data.emails = {{
                    .key = {},
                    .value = "undo-redo@example.invalid",
                    .label = std::optional<std::string>{"test"},
                    .preference = 1,
                    .contexts = {{"work", true}},
                }};
                data.phones = {{
                    .key = {},
                    .value = "+64 9 555 0100",
                    .label = std::nullopt,
                    .preference = std::nullopt,
                    .contexts = {{"work", true}},
                }};
                data.addresses = {{
                    .key = {},
                    .value = "1 Undo Lane, Test City",
                    .label = std::nullopt,
                    .preference = std::nullopt,
                    .contexts = {{"work", true}},
                }};
                data.birthday = "2000-01-02";
                data.notes = "Autonomous undo/redo fixture";
            }
            auto after = javelin::jmap::contacts::applyContactEditorData(data, false);
            if (const auto* error = std::get_if<std::string_view>(&after))
                throw std::runtime_error(std::string{*error});

            auto entry = contactEntry(
                context, {
                             .addressBookId = context.addressBookId,
                             .currentCardId = before->id,
                             .uid = uid,
                             .beforeDocumentJson = before->document,
                             .afterDocumentJson = std::get<std::string>(std::move(after)),
                         });
            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                     groupMembership ? "group membership add"
                                                     : "contact multi-field edit");
            auto edited = co_await findContact(context, uid);
            if (!edited.has_value())
                throw std::runtime_error("edited contact disappeared");
            auto editedData = javelin::jmap::contacts::contactEditorData(edited->document);
            if (!std::holds_alternative<javelin::jmap::contacts::ContactEditorData>(editedData))
                throw std::runtime_error("edited contact is not parseable");
            const auto& projected =
                std::get<javelin::jmap::contacts::ContactEditorData>(editedData);
            if (groupMembership)
            {
                if (!std::ranges::contains(projected.members, *memberUid))
                    throw std::runtime_error("group member was not added");
            }
            else if (projected.organization != "Javelin Undo Lab" ||
                     projected.title != "Round-trip tester" || projected.emails.size() != 1 ||
                     projected.phones.size() != 1 || projected.addresses.size() != 1)
            {
                throw std::runtime_error("multi-field contact edit was incomplete");
            }

            entry =
                co_await execute(context, std::move(entry), HistoryExecutionDirection::Undo,
                                 groupMembership ? "group membership undo" : "contact edit undo");
            auto undone = co_await findContact(context, uid);
            if (!undone.has_value())
                throw std::runtime_error("contact disappeared after edit undo");
            auto undoneData = javelin::jmap::contacts::contactEditorData(undone->document);
            if (!std::holds_alternative<javelin::jmap::contacts::ContactEditorData>(undoneData))
                throw std::runtime_error("undone contact is not parseable");
            const auto& restored = std::get<javelin::jmap::contacts::ContactEditorData>(undoneData);
            if (groupMembership)
            {
                if (std::ranges::contains(restored.members, *memberUid))
                    throw std::runtime_error("group member remained after undo");
            }
            else if (!restored.organization.empty() || !restored.title.empty() ||
                     !restored.emails.empty() || !restored.phones.empty() ||
                     !restored.addresses.empty())
            {
                throw std::runtime_error("multi-field contact edit did not fully undo");
            }

            entry =
                co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                 groupMembership ? "group membership redo" : "contact edit redo");
            static_cast<void>(entry);
        }

        [[nodiscard]] QCoro::Task<void> deleteRoundTrip(ContactFixtureContext& context,
                                                        const std::string& uid,
                                                        const std::string_view label)
        {
            const auto before = co_await findContact(context, uid);
            if (!before.has_value())
                co_return;
            auto entry = contactEntry(context, {
                                                   .addressBookId = context.addressBookId,
                                                   .currentCardId = before->id,
                                                   .uid = uid,
                                                   .beforeDocumentJson = before->document,
                                                   .afterDocumentJson = std::nullopt,
                                               });
            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                     std::string{label} + " delete");
            if ((co_await findContact(context, uid)).has_value())
                throw std::runtime_error(std::string{label} + " remained after delete");
            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Undo,
                                     std::string{label} + " delete undo");
            if (!(co_await findContact(context, uid)).has_value())
                throw std::runtime_error(std::string{label} + " was not recreated by undo");
            entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                     std::string{label} + " delete redo");
            if ((co_await findContact(context, uid)).has_value())
                throw std::runtime_error(std::string{label} + " remained after delete redo");
        }

        [[nodiscard]] QCoro::Task<void> cleanupContacts(ContactFixtureContext& context)
        {
            auto authoritative = co_await contacts(context);
            javelin::jmap::api::ContactCardSetRequest request{
                .accountId = context.accountId,
                .ifInState =
                    authoritative.state.empty() ? std::nullopt : std::optional{authoritative.state},
                .create = {},
                .update = {},
                .destroy = {},
            };
            for (const auto& contact : authoritative.contacts)
                if (std::ranges::contains(context.fixtureUids, contact.uid) ||
                    contact.displayName.starts_with("Javelin undo lab "))
                    request.destroy.push_back(contact.id);
            if (request.destroy.empty())
                co_return;
            auto removed = co_await context.port.applyContactCardsFromHistory(
                context.connectionId, std::move(request), CommandOrigin::SystemChild);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&removed))
                throw operationFailure("contact cleanup", *error);
        }

        [[nodiscard]] QCoro::Task<ContactFixtureContext>
        contactContext(javelin::app::DaemonServices& services,
                       const AutonomousSuiteAccount& account, const std::string& runTag)
        {
            auto* contactPort = dynamic_cast<ContactHistoryPort*>(&services.contactCommandPort());
            auto* addressBookPort = dynamic_cast<javelin::app::undo::AddressBookHistoryPort*>(
                &services.contactCommandPort());
            if (contactPort == nullptr || addressBookPort == nullptr)
                throw std::runtime_error("contact history ports are unavailable");
            std::optional<javelin::jmap::OperationError> lastError;
            for (const auto& accountId : account.accountIds)
            {
                auto loaded = co_await addressBookPort->getAuthoritativeAddressBooks(
                    account.connectionId, accountId);
                if (const auto* books =
                        std::get_if<javelin::app::undo::AuthoritativeAddressBooks>(&loaded))
                {
                    const auto writable =
                        std::ranges::find_if(books->addressBooks, [](const auto& book)
                                             { return book.myRights.mayWrite; });
                    if (writable != books->addressBooks.end())
                    {
                        co_return ContactFixtureContext{
                            .connectionId = account.connectionId,
                            .accountId = accountId,
                            .addressBookId = writable->id,
                            .runTag = runTag,
                            .port = *contactPort,
                            .executor = ContactHistoryExecutor{*contactPort},
                            .journal =
                                javelin::jmap::sync::MutationJournalRepository{
                                    services.databaseConnection()},
                            .fixtureUids = {},
                        };
                    }
                }
                else
                    lastError = std::get<javelin::jmap::OperationError>(std::move(loaded));
            }
            if (lastError.has_value())
                throw operationFailure("load contact address books", *lastError);
            throw std::runtime_error("no writable contact address book is available");
        }

        [[nodiscard]] QCoro::Task<void> runContacts(javelin::app::DaemonServices& services,
                                                    const AutonomousSuiteAccount& account,
                                                    const std::string& runTag)
        {
            auto context = co_await contactContext(services, account, runTag);
            const auto personUid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            const auto groupUid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            context.fixtureUids = {personUid, groupUid};
            std::exception_ptr failure;
            try
            {
                static_cast<void>(co_await createRoundTrip(
                    context, personUid, contactDocument(context, personUid, runTag + " contact"),
                    "contact"));
                co_await editRoundTrip(context, personUid, false, std::nullopt);

                auto groupDocument = javelin::jmap::contacts::createContactGroupDocument(
                    runTag + " group", groupUid, context.addressBookId);
                if (const auto* error = std::get_if<std::string_view>(&groupDocument))
                    throw std::runtime_error(std::string{*error});
                static_cast<void>(co_await createRoundTrip(
                    context, groupUid, std::get<std::string>(std::move(groupDocument)), "group"));
                co_await editRoundTrip(context, groupUid, true, personUid);
                co_await deleteRoundTrip(context, groupUid, "group");
                co_await deleteRoundTrip(context, personUid, "contact");
                std::cout << "PASS\tautonomous-contact\tcreate/edit/delete and group membership\n";
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            try
            {
                co_await cleanupContacts(context);
            }
            catch (const std::exception& cleanupError)
            {
                std::cerr << "CLEANUP FAILED\tautonomous-contact\t" << cleanupError.what() << '\n';
                if (!failure)
                    throw;
            }
            if (failure)
                std::rethrow_exception(failure);
        }

        [[nodiscard]] QCoro::Task<AuthoritativeAddressBooks>
        addressBooks(AddressBookFixtureContext& context)
        {
            auto loaded = co_await context.books.getAuthoritativeAddressBooks(context.connectionId,
                                                                              context.accountId);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                throw operationFailure("refresh address books", *error);
            co_return std::get<AuthoritativeAddressBooks>(std::move(loaded));
        }

        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::api::AddressBook>>
        findAddressBook(AddressBookFixtureContext& context)
        {
            auto authoritative = co_await addressBooks(context);
            const auto found =
                std::ranges::find_if(authoritative.addressBooks, [&context](const auto& book)
                                     { return book.name.starts_with(context.runTag); });
            if (found == authoritative.addressBooks.end())
                co_return std::nullopt;
            co_return *found;
        }

        [[nodiscard]] HistoryEntry addressBookEntry(const AddressBookFixtureContext& context,
                                                    AddressBookHistory history)
        {
            HistoryEntry entry;
            entry.commandKind = QStringLiteral("address_book");
            history.connectionId = context.connectionId;
            history.accountId = context.accountId;
            entry.payload = std::move(history);
            return entry;
        }

        [[nodiscard]] QCoro::Task<HistoryEntry> execute(AddressBookFixtureContext& context,
                                                        HistoryEntry entry,
                                                        const HistoryExecutionDirection direction,
                                                        const std::string_view step)
        {
            auto result = co_await context.executor.execute(entry, direction);
            if (result.outcome != HistoryExecutionOutcome::Success ||
                !result.updatedPayload.has_value())
                throw std::runtime_error(std::string{step} + ": " + result.summary.toStdString());
            entry.payload = std::move(*result.updatedPayload);
            const auto active = context.journal.listActive({
                .accountId = context.accountId,
                .dataType = "AddressBook",
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
                throw std::runtime_error(error->message.toStdString());
            if (!std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active).empty())
                throw std::runtime_error(std::string{step} +
                                         ": optimistic address-book journal did not drain");
            co_return entry;
        }

        [[nodiscard]] QCoro::Task<void> cleanupAddressBooks(AddressBookFixtureContext& context)
        {
            auto authoritative = co_await addressBooks(context);
            javelin::jmap::api::AddressBookSetRequest request{
                .accountId = context.accountId,
                .ifInState =
                    authoritative.state.empty() ? std::nullopt : std::optional{authoritative.state},
                .create = {},
                .update = {},
                .destroy = {},
                .onDestroyRemoveContents = false,
                .onSuccessSetIsDefault = std::nullopt,
            };
            for (const auto& book : authoritative.addressBooks)
                if (book.name.starts_with(context.runTag))
                    request.destroy.push_back(book.id);
            if (request.destroy.empty())
                co_return;
            auto removed = co_await context.books.applyAddressBooksFromHistory(
                context.connectionId, std::move(request), CommandOrigin::SystemChild);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&removed))
                throw operationFailure("address-book cleanup", *error);
        }

        [[nodiscard]] QCoro::Task<void> runAddressBooks(javelin::app::DaemonServices& services,
                                                        const AutonomousSuiteAccount& account,
                                                        const std::string& accountId,
                                                        const std::string& runTag)
        {
            auto* contacts = dynamic_cast<ContactHistoryPort*>(&services.contactCommandPort());
            auto* books = dynamic_cast<AddressBookHistoryPort*>(&services.contactCommandPort());
            if (contacts == nullptr || books == nullptr)
                throw std::runtime_error("address-book history ports are unavailable");
            AddressBookFixtureContext context{
                .connectionId = account.connectionId,
                .accountId = accountId,
                .runTag = runTag,
                .books = *books,
                .contacts = *contacts,
                .executor = AddressBookHistoryExecutor{*books, *contacts},
                .journal =
                    javelin::jmap::sync::MutationJournalRepository{services.databaseConnection()},
            };
            std::exception_ptr failure;
            try
            {
                javelin::jmap::api::AddressBook created{
                    .id = {},
                    .name = runTag + " address book",
                    .description = std::optional<std::string>{"Autonomous undo/redo fixture"},
                    .sortOrder = 701,
                    .isDefault = false,
                    .isSubscribed = true,
                    .shareWith = std::nullopt,
                    .myRights = {},
                };
                const auto createdJson = javelin::jmap::api::serializeAddressBookDocument(created);
                if (!createdJson.has_value())
                    throw std::runtime_error("unable to serialize address-book fixture");
                auto entry =
                    addressBookEntry(context, {
                                                  .connectionId = {},
                                                  .accountId = {},
                                                  .currentAddressBookId = std::nullopt,
                                                  .beforeDocumentJson = std::nullopt,
                                                  .afterDocumentJson = *createdJson,
                                                  .beforeDefaultAddressBookId = std::nullopt,
                                                  .afterDefaultAddressBookId = std::nullopt,
                                                  .affectedCards = {},
                                              });
                entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                         "address-book create");
                if (!(co_await findAddressBook(context)).has_value())
                    throw std::runtime_error("address book was not created");
                entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Undo,
                                         "address-book create undo");
                if ((co_await findAddressBook(context)).has_value())
                    throw std::runtime_error("address book remained after create undo");
                entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                         "address-book create redo");

                const auto before = co_await findAddressBook(context);
                if (!before.has_value())
                    throw std::runtime_error("address book disappeared before edit");
                auto edited = *before;
                edited.name = runTag + " renamed address book";
                edited.description = "Edited autonomous fixture";
                edited.sortOrder = 702;
                const auto beforeJson = javelin::jmap::api::serializeAddressBookDocument(*before);
                const auto afterJson = javelin::jmap::api::serializeAddressBookDocument(edited);
                if (!beforeJson.has_value() || !afterJson.has_value())
                    throw std::runtime_error("unable to serialize address-book edit");
                auto editEntry =
                    addressBookEntry(context, {
                                                  .connectionId = {},
                                                  .accountId = {},
                                                  .currentAddressBookId = before->id,
                                                  .beforeDocumentJson = *beforeJson,
                                                  .afterDocumentJson = *afterJson,
                                                  .beforeDefaultAddressBookId = std::nullopt,
                                                  .afterDefaultAddressBookId = std::nullopt,
                                                  .affectedCards = {},
                                              });
                editEntry = co_await execute(context, std::move(editEntry),
                                             HistoryExecutionDirection::Redo, "address-book edit");
                editEntry =
                    co_await execute(context, std::move(editEntry), HistoryExecutionDirection::Undo,
                                     "address-book edit undo");
                editEntry =
                    co_await execute(context, std::move(editEntry), HistoryExecutionDirection::Redo,
                                     "address-book edit redo");

                const auto deleting = co_await findAddressBook(context);
                if (!deleting.has_value())
                    throw std::runtime_error("address book disappeared before delete");
                const auto deletingJson =
                    javelin::jmap::api::serializeAddressBookDocument(*deleting);
                if (!deletingJson.has_value())
                    throw std::runtime_error("unable to serialize address-book delete");
                auto deleteEntry =
                    addressBookEntry(context, {
                                                  .connectionId = {},
                                                  .accountId = {},
                                                  .currentAddressBookId = deleting->id,
                                                  .beforeDocumentJson = *deletingJson,
                                                  .afterDocumentJson = std::nullopt,
                                                  .beforeDefaultAddressBookId = std::nullopt,
                                                  .afterDefaultAddressBookId = std::nullopt,
                                                  .affectedCards = {},
                                              });
                deleteEntry =
                    co_await execute(context, std::move(deleteEntry),
                                     HistoryExecutionDirection::Redo, "address-book delete");
                deleteEntry =
                    co_await execute(context, std::move(deleteEntry),
                                     HistoryExecutionDirection::Undo, "address-book delete undo");
                deleteEntry =
                    co_await execute(context, std::move(deleteEntry),
                                     HistoryExecutionDirection::Redo, "address-book delete redo");
                if ((co_await findAddressBook(context)).has_value())
                    throw std::runtime_error("address book remained after delete redo");
                std::cout << "PASS\tautonomous-address-book\tcreate/edit/delete\n";
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            try
            {
                co_await cleanupAddressBooks(context);
            }
            catch (const std::exception& cleanupError)
            {
                std::cerr << "CLEANUP FAILED\tautonomous-address-book\t" << cleanupError.what()
                          << '\n';
                if (!failure)
                    throw;
            }
            if (failure)
                std::rethrow_exception(failure);
        }

        [[nodiscard]] QCoro::Task<std::optional<javelin::jmap::calendar::CalendarEvent>>
        calendarEvent(CalendarFixtureContext& context,
                      const std::optional<std::string>& eventId = std::nullopt)
        {
            auto loaded = co_await context.port.getAuthoritativeCalendarEvent(
                context.connectionId, context.accountId, eventId, context.uid);
            if (const auto* error = std::get_if<javelin::jmap::OperationError>(&loaded))
                throw operationFailure("refresh calendar event", *error);
            co_return std::get<javelin::jmap::calendar::AuthoritativeCalendarEvent>(
                std::move(loaded))
                .event;
        }

        [[nodiscard]] HistoryEntry calendarEntry(const CalendarFixtureContext& context,
                                                 CalendarEventHistory history)
        {
            HistoryEntry entry;
            entry.commandKind = QStringLiteral("calendar_event");
            history.connectionId = context.connectionId;
            history.accountId = context.accountId;
            history.calendarId = context.calendarId;
            history.uid = context.uid;
            entry.payload = std::move(history);
            return entry;
        }

        [[nodiscard]] QCoro::Task<HistoryEntry> execute(CalendarFixtureContext& context,
                                                        HistoryEntry entry,
                                                        const HistoryExecutionDirection direction,
                                                        const std::string_view step)
        {
            auto result = co_await context.executor.execute(entry, direction);
            if (result.outcome != HistoryExecutionOutcome::Success ||
                !result.updatedPayload.has_value())
                throw std::runtime_error(std::string{step} + ": " + result.summary.toStdString());
            entry.payload = std::move(*result.updatedPayload);
            const auto active = context.journal.listActive({
                .accountId = context.accountId,
                .dataType = "CalendarEvent",
            });
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&active))
                throw std::runtime_error(error->message.toStdString());
            const auto& records =
                std::get<std::vector<javelin::jmap::sync::MutationRecord>>(active);
            if (std::ranges::any_of(records, [&context](const auto& record)
                                    { return record.payloadJson.contains(context.uid); }))
                throw std::runtime_error(std::string{step} +
                                         ": optimistic calendar journal did not drain");
            co_return entry;
        }

        [[nodiscard]] QCoro::Task<void> cleanupCalendar(CalendarFixtureContext& context)
        {
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const auto event = co_await calendarEvent(context);
                if (!event.has_value())
                    co_return;
                auto removed = co_await context.port.deleteCalendarEvent(
                    context.connectionId,
                    {
                        .accountId = context.accountId,
                        .eventId = event->id,
                        .calendarIds = {context.calendarId},
                        .operationGroupId = std::nullopt,
                        .ifInState = std::nullopt,
                    },
                    CommandOrigin::SystemChild);
                if (!std::holds_alternative<javelin::jmap::OperationError>(removed))
                    co_return;
                if (attempt == 2)
                    throw operationFailure(
                        "calendar cleanup",
                        std::get<javelin::jmap::OperationError>(std::move(removed)));
            }
        }

        [[nodiscard]] QCoro::Task<void> runCalendar(javelin::app::DaemonServices& services,
                                                    const AutonomousSuiteAccount& account,
                                                    const std::string& runTag)
        {
            std::optional<std::pair<std::string, std::string>> target;
            for (const auto& accountId : account.accountIds)
            {
                auto calendars = services.calendarReader().calendars(accountId);
                if (const auto* list =
                        std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&calendars))
                {
                    const auto writable = std::ranges::find_if(
                        *list, [](const auto& calendar)
                        { return calendar.myRights.mayWriteAll || calendar.myRights.mayWriteOwn; });
                    if (writable != list->end())
                    {
                        target = std::pair{accountId, writable->id};
                        break;
                    }
                }
            }
            if (!target.has_value())
                throw std::runtime_error("no writable calendar is available");
            CalendarFixtureContext context{
                // Calendar history stores the owner account id in this field, matching
                // CalendarHistoryPort deliberately uses the stable series identity for recovery.
                .connectionId = account.accountIds.front(),
                .accountId = target->first,
                .calendarId = target->second,
                .uid = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),
                .runTag = runTag,
                .port = services.calendarApplicationService(),
                .executor = CalendarHistoryExecutor{services.calendarApplicationService()},
                .journal =
                    javelin::jmap::sync::MutationJournalRepository{services.databaseConnection()},
            };
            std::exception_ptr failure;
            try
            {
                javelin::jmap::calendar::CalendarEvent created{
                    .accountId = context.accountId,
                    .id = {},
                    .baseEventId = std::nullopt,
                    .recurrenceId = std::nullopt,
                    .uid = context.uid,
                    .calendarIds = {{context.calendarId, true}},
                    .title = runTag + " calendar event",
                    .description = std::optional<std::string>{"Autonomous undo/redo fixture"},
                    .location = std::optional<std::string>{"Undo Lab"},
                    .start = {.value = "2031-02-03T10:15:00"},
                    .duration = {.value = "PT45M"},
                    .timeZone =
                        std::optional<javelin::jmap::calendar::TimeZoneId>{
                            {.value = "Pacific/Auckland"}},
                    .showWithoutTime = false,
                    .isDraft = false,
                    .isOrigin = true,
                    .useDefaultAlerts = false,
                    .alerts = {},
                    .utcStart = std::nullopt,
                    .utcEnd = std::nullopt,
                    .recurrenceRule = std::nullopt,
                    .recurrenceOverrides = {},
                    .attendees = {},
                };
                const auto createdJson =
                    javelin::jmap::api::serializeCalendarEventDocument(created);
                if (!createdJson.has_value())
                    throw std::runtime_error("unable to serialize calendar fixture");
                auto entry = calendarEntry(context, {
                                                        .connectionId = {},
                                                        .accountId = {},
                                                        .calendarId = {},
                                                        .currentEventId = std::nullopt,
                                                        .uid = {},
                                                        .beforeDocumentJson = std::nullopt,
                                                        .afterDocumentJson = *createdJson,
                                                    });
                entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                         "calendar create");
                if (!(co_await calendarEvent(context)).has_value())
                    throw std::runtime_error("calendar event was not created");
                entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Undo,
                                         "calendar create undo");
                if ((co_await calendarEvent(context)).has_value())
                    throw std::runtime_error("calendar event remained after create undo");
                entry = co_await execute(context, std::move(entry), HistoryExecutionDirection::Redo,
                                         "calendar create redo");

                const auto before = co_await calendarEvent(context);
                if (!before.has_value())
                    throw std::runtime_error("calendar event disappeared before edit");
                auto after = *before;
                after.title = runTag + " edited calendar event";
                after.description = "Edited description";
                after.location = "Redo Lab";
                after.start.value = "2031-02-03T11:30:00";
                after.duration.value = "PT1H15M";
                const auto beforeJson = javelin::jmap::api::serializeCalendarEventDocument(*before);
                const auto afterJson = javelin::jmap::api::serializeCalendarEventDocument(after);
                if (!beforeJson.has_value() || !afterJson.has_value())
                    throw std::runtime_error("unable to serialize calendar edit");
                auto editEntry = calendarEntry(context, {
                                                            .connectionId = {},
                                                            .accountId = {},
                                                            .calendarId = {},
                                                            .currentEventId = before->id,
                                                            .uid = {},
                                                            .beforeDocumentJson = *beforeJson,
                                                            .afterDocumentJson = *afterJson,
                                                        });
                editEntry = co_await execute(context, std::move(editEntry),
                                             HistoryExecutionDirection::Redo, "calendar edit");
                editEntry = co_await execute(context, std::move(editEntry),
                                             HistoryExecutionDirection::Undo, "calendar edit undo");
                editEntry = co_await execute(context, std::move(editEntry),
                                             HistoryExecutionDirection::Redo, "calendar edit redo");

                const auto deleting = co_await calendarEvent(context);
                if (!deleting.has_value())
                    throw std::runtime_error("calendar event disappeared before delete");
                const auto deletingJson =
                    javelin::jmap::api::serializeCalendarEventDocument(*deleting);
                if (!deletingJson.has_value())
                    throw std::runtime_error("unable to serialize calendar delete");
                auto deleteEntry = calendarEntry(context, {
                                                              .connectionId = {},
                                                              .accountId = {},
                                                              .calendarId = {},
                                                              .currentEventId = deleting->id,
                                                              .uid = {},
                                                              .beforeDocumentJson = *deletingJson,
                                                              .afterDocumentJson = std::nullopt,
                                                          });
                deleteEntry = co_await execute(context, std::move(deleteEntry),
                                               HistoryExecutionDirection::Redo, "calendar delete");
                deleteEntry =
                    co_await execute(context, std::move(deleteEntry),
                                     HistoryExecutionDirection::Undo, "calendar delete undo");
                deleteEntry =
                    co_await execute(context, std::move(deleteEntry),
                                     HistoryExecutionDirection::Redo, "calendar delete redo");
                if ((co_await calendarEvent(context)).has_value())
                    throw std::runtime_error("calendar event remained after delete redo");
                std::cout << "PASS\tautonomous-calendar\tcreate/edit/delete\n";
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            try
            {
                co_await cleanupCalendar(context);
            }
            catch (const std::exception& cleanupError)
            {
                std::cerr << "CLEANUP FAILED\tautonomous-calendar\t" << cleanupError.what() << '\n';
                if (!failure)
                    throw;
            }
            if (failure)
                std::rethrow_exception(failure);
        }

        [[nodiscard]] QCoro::Task<void> runDraft(javelin::app::DaemonServices& services,
                                                 const AutonomousSuiteAccount& account,
                                                 const std::string& runTag)
        {
            std::optional<std::pair<std::string, std::string>> target;
            for (const auto& accountId : account.accountIds)
            {
                const auto identities = services.identityRepository().listByAccount(accountId);
                if (const auto* values =
                        std::get_if<std::vector<javelin::jmap::domain::Identity>>(&identities);
                    values != nullptr && !values->empty())
                {
                    target = std::pair{accountId, values->front().id};
                    break;
                }
            }
            if (!target.has_value())
                throw std::runtime_error("no mail identity is available for a draft fixture");

            const auto composeSessionId =
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
            javelin::jmap::submission::DraftSnapshot created{
                .composeSessionId = composeSessionId,
                .accountId = target->first,
                .draftEmailId = std::nullopt,
                .mode = javelin::jmap::submission::ComposeMode::NewMessage,
                .editorMode = javelin::jmap::submission::BodyEditorMode::RichText,
                .identityId = target->second,
                .to = {},
                .cc = {},
                .bcc = {},
                .subject = runTag + " draft",
                .plainTextBody = "Autonomous undo/redo fixture",
                .htmlBody = "<p>Autonomous undo/redo fixture</p>",
                .threading = {},
                .attachments = {},
            };
            DraftHistoryExecutor executor{services.composeService()};
            HistoryEntry entry;
            entry.commandKind = QStringLiteral("draft");
            entry.payload = DraftHistory{
                .connectionId = account.connectionId,
                .accountId = target->first,
                .composeSessionId = composeSessionId,
                .currentDraftEmailId = std::nullopt,
                .beforeSnapshotJson = std::nullopt,
                .afterSnapshotJson =
                    javelin::jmap::submission::serializeDraftSnapshot(created).toStdString(),
            };
            std::exception_ptr failure;
            try
            {
                const auto apply =
                    [&executor](HistoryEntry value, const HistoryExecutionDirection direction,
                                const std::string_view step) -> QCoro::Task<HistoryEntry>
                {
                    auto result = co_await executor.execute(value, direction);
                    if (result.outcome != HistoryExecutionOutcome::Success ||
                        !result.updatedPayload.has_value())
                        throw std::runtime_error(std::string{step} + ": " +
                                                 result.summary.toStdString());
                    value.payload = std::move(*result.updatedPayload);
                    co_return value;
                };
                entry = co_await apply(entry, HistoryExecutionDirection::Redo, "draft create");
                entry = co_await apply(entry, HistoryExecutionDirection::Undo, "draft create undo");
                entry = co_await apply(entry, HistoryExecutionDirection::Redo, "draft create redo");

                auto& history = std::get<DraftHistory>(entry.payload);
                auto before = co_await services.composeService().loadAuthoritativeDraft(
                    history.accountId, *history.currentDraftEmailId, history.composeSessionId);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&before))
                    throw operationFailure("load draft fixture", *error);
                auto edited = std::get<javelin::jmap::submission::DraftSnapshot>(std::move(before));
                history.beforeSnapshotJson =
                    javelin::jmap::submission::serializeDraftSnapshot(edited).toStdString();
                edited.subject = runTag + " edited draft";
                edited.plainTextBody = "Edited autonomous undo/redo fixture";
                edited.htmlBody = "<p>Edited autonomous undo/redo fixture</p>";
                history.afterSnapshotJson =
                    javelin::jmap::submission::serializeDraftSnapshot(edited).toStdString();
                entry = co_await apply(entry, HistoryExecutionDirection::Redo, "draft edit");
                entry = co_await apply(entry, HistoryExecutionDirection::Undo, "draft edit undo");
                entry = co_await apply(entry, HistoryExecutionDirection::Redo, "draft edit redo");
                std::cout << "PASS\tautonomous-draft\tcreate/edit\n";
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            const auto& history = std::get<DraftHistory>(entry.payload);
            if (history.currentDraftEmailId.has_value())
            {
                auto removed = co_await services.composeService().deleteDraftFromHistory(
                    history.accountId, *history.currentDraftEmailId, CommandOrigin::SystemChild);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&removed))
                {
                    std::cerr << "CLEANUP FAILED\tautonomous-draft\t"
                              << error->message.toStdString() << '\n';
                    if (!failure)
                        throw operationFailure("draft cleanup", *error);
                }
            }
            if (failure)
                std::rethrow_exception(failure);
        }
    } // namespace

    QCoro::Task<int> runUndoRedoAutonomousSuite(javelin::app::DaemonServices& services,
                                                AutonomousSuiteAccount account)
    {
        const auto runTag =
            std::string{"Javelin undo lab "} +
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(8).toStdString();
        int failures = 0;
        std::optional<std::string> contactAccountId;
        try
        {
            auto context = co_await contactContext(services, account, runTag);
            contactAccountId = context.accountId;
            co_await runContacts(services, account, runTag);
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL\tautonomous-contact\t" << error.what() << '\n';
        }
        if (contactAccountId.has_value())
        {
            try
            {
                co_await runAddressBooks(services, account, *contactAccountId, runTag);
            }
            catch (const std::exception& error)
            {
                ++failures;
                std::cerr << "FAIL\tautonomous-address-book\t" << error.what() << '\n';
            }
        }
        try
        {
            co_await runCalendar(services, account, runTag);
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL\tautonomous-calendar\t" << error.what() << '\n';
        }
        try
        {
            co_await runDraft(services, account, runTag);
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL\tautonomous-draft\t" << error.what() << '\n';
        }
        co_return failures == 0 ? 0 : 1;
    }
} // namespace javelin::tools
