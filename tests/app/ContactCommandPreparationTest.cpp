#include "app/ContactCommandPreparation.h"
#include "jmap/api/PatchObject.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{
    [[nodiscard]] javelin::jmap::api::AddressBook editableAddressBook()
    {
        return javelin::jmap::api::AddressBook{
            .id = "book-1",
            .name = "Friends",
            .description = std::nullopt,
            .sortOrder = 10,
            .isDefault = false,
            .isSubscribed = true,
            .shareWith = std::nullopt,
            .myRights =
                {
                    .mayRead = true,
                    .mayWrite = true,
                    .mayShare = true,
                    .mayDelete = true,
                },
        };
    }

    [[nodiscard]] javelin::jmap::contacts::ContactEditorData
    editableContact(std::string uid = "contact-uid")
    {
        return javelin::jmap::contacts::ContactEditorData{
            .uid = std::move(uid),
            .kind = "individual",
            .fullName = "Alice Example",
            .organization = {},
            .title = {},
            .emails = {},
            .phones = {},
            .addresses = {},
            .members = {},
            .birthday = {},
            .notes = {},
            .addressBookIds = {"book-1"},
            .document =
                R"({"id":"server-id","uid":"contact-uid","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Old Name"}})",
        };
    }
} // namespace

TEST_CASE("address book commands prepare set requests outside the GUI", "[app][contacts]")
{
    auto create = javelin::app::prepareAddressBookMutation(javelin::app::CreateAddressBookCommand{
        .accountId = "contacts-account",
        .addressBook = editableAddressBook(),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::AddressBookSetRequest>(create));
    const auto& createRequest = std::get<javelin::jmap::api::AddressBookSetRequest>(create);
    CHECK(createRequest.accountId == "contacts-account");
    CHECK(createRequest.create.contains("new-address-book"));

    auto remove = javelin::app::prepareAddressBookMutation(javelin::app::DeleteAddressBookCommand{
        .accountId = "contacts-account",
        .addressBookId = "book-1",
        .removeContents = true,
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::AddressBookSetRequest>(remove));
    const auto& removeRequest = std::get<javelin::jmap::api::AddressBookSetRequest>(remove);
    CHECK(removeRequest.destroy == std::vector<std::string>{"book-1"});
    CHECK(removeRequest.onDestroyRemoveContents);
}

TEST_CASE("new contacts receive a uid during command preparation", "[app][contacts]")
{
    auto contact = editableContact({});
    contact.document = R"({"kind":"individual","name":{"full":""}})";
    auto prepared = javelin::app::prepareSaveContact({
        .accountId = "a1",
        .contactId = std::nullopt,
        .contact = std::move(contact),
    });

    const auto* request = std::get_if<javelin::jmap::api::ContactCardSetRequest>(&prepared);
    REQUIRE(request != nullptr);
    REQUIRE(request->create.contains("new-contact"));
    CHECK(request->create.at("new-contact").json.find("\"uid\"") != std::string::npos);
}

TEST_CASE("contact save commands prepare protocol requests outside the GUI", "[app][contacts]")
{
    auto create = javelin::app::prepareSaveContact({
        .accountId = "contacts-account",
        .contactId = std::nullopt,
        .contact = editableContact(),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::ContactCardSetRequest>(create));
    const auto& createRequest = std::get<javelin::jmap::api::ContactCardSetRequest>(create);
    REQUIRE(createRequest.create.size() == 1);
    const auto& createDocument = createRequest.create.at("new-contact").json;
    CHECK(createDocument.find("Alice Example") != std::string::npos);
    CHECK(createDocument.find(R"("id")") == std::string::npos);

    auto update = javelin::app::prepareSaveContact({
        .accountId = "contacts-account",
        .contactId = "contact-1",
        .contact = editableContact(),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::ContactCardSetRequest>(update));
    CHECK(std::get<javelin::jmap::api::ContactCardSetRequest>(update).update.contains("contact-1"));
}

TEST_CASE("clearing a contact field prepares an explicit removal patch", "[app][contacts]")
{
    auto contact = editableContact();
    contact.document =
        R"({"id":"contact-1","uid":"contact-uid","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Alice Example"},"emails":{"test":{"address":"old@example.test","label":"Test"}}})";
    contact.emails.clear();
    auto prepared = javelin::app::prepareSaveContact({
        .accountId = "contacts-account",
        .contactId = "contact-1",
        .contact = std::move(contact),
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::ContactCardSetRequest>(prepared));
    const auto& patch =
        std::get<javelin::jmap::api::ContactCardSetRequest>(prepared).update.at("contact-1").json;
    CHECK(patch.find("emails") != std::string::npos);
    CHECK(patch.find("null") != std::string::npos);

    const auto projected = javelin::jmap::api::applyPatchObject(
        R"({"@type":"Card","version":"1.0","uid":"contact-uid","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Alice Example"},"emails":{"test":{"address":"old@example.test","label":"Test"}}})",
        patch);
    REQUIRE(std::holds_alternative<std::string>(projected));
    CHECK(std::get<std::string>(projected).find(R"("emails")") == std::string::npos);
}

TEST_CASE("contact commands validate documents before protocol submission", "[app][contacts]")
{
    auto result = javelin::app::prepareSetContactsStarred({
        .accountId = "contacts-account",
        .contacts = {{.id = "contact-1", .document = "not-json"}},
        .starred = true,
    });
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).code ==
          javelin::jmap::OperationErrorCode::InvalidUserInput);
}

TEST_CASE("contact group deletion prepares a typed destroy request", "[app][contacts][groups]")
{
    auto result = javelin::app::prepareDeleteContactGroup({
        .accountId = "contacts-account",
        .groupId = "group-1",
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::ContactCardSetRequest>(result));
    const auto& request = std::get<javelin::jmap::api::ContactCardSetRequest>(result);
    CHECK(request.accountId == "contacts-account");
    CHECK(request.destroy == std::vector<std::string>{"group-1"});
}

TEST_CASE("contact copy commands escape protocol documents safely", "[app][contacts]")
{
    auto result = javelin::app::prepareCopyContact({
        .sourceAccountId = "source-account",
        .destinationAccountId = "destination-account",
        .contactId = "contact-\"one",
        .destinationAddressBookId = "book\\one",
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::ContactCardCopyRequest>(result));
    const auto& request = std::get<javelin::jmap::api::ContactCardCopyRequest>(result);
    REQUIRE(request.create.size() == 1);
    const auto& document = request.create.at("copy-contact").json;
    CHECK(document.find(R"(contact-\"one)") != std::string::npos);
    CHECK(document.find(R"(book\\one)") != std::string::npos);
}

TEST_CASE("contact import commands reject duplicate cached UIDs", "[app][contacts]")
{
    auto result = javelin::app::prepareImportContacts({
        .accountId = "contacts-account",
        .addressBookId = "book-1",
        .contacts = {editableContact("duplicate-uid")},
        .knownUids = {"duplicate-uid"},
    });
    REQUIRE(std::holds_alternative<javelin::jmap::OperationError>(result));
    CHECK(std::get<javelin::jmap::OperationError>(result).message.contains(
        QStringLiteral("already present")));
}

TEST_CASE("contact merge commands update the primary and destroy duplicates", "[app][contacts]")
{
    auto result = javelin::app::prepareMergeContacts({
        .accountId = "contacts-account",
        .primary =
            {.id = "contact-1",
             .document =
                 R"({"uid":"one","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Alice"}})"},
        .duplicates =
            {{.id = "contact-2",
              .document =
                  R"({"uid":"two","kind":"individual","addressBookIds":{"book-1":true},"emails":{"email-1":{"address":"alice@example.test"}}})"}},
    });
    REQUIRE(std::holds_alternative<javelin::jmap::api::ContactCardSetRequest>(result));
    const auto& request = std::get<javelin::jmap::api::ContactCardSetRequest>(result);
    REQUIRE(request.destroy == std::vector<std::string>{"contact-2"});
    REQUIRE(request.update.contains("contact-1"));
    CHECK(request.update.at("contact-1").json.find("alice@example.test") != std::string::npos);
}
