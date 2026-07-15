#include "jmap/api/ContactsMethods.h"
#include "jmap/contacts/ContactTypes.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("contacts methods parse address books and preserve complete contact cards",
          "[jmap][contacts][protocol]")
{
    const auto books = javelin::jmap::api::parseAddressBookGetResponse(
        R"({"accountId":"a0x9","state":"~4144","list":[{"id":"book-1","name":"Personal","description":null,"sortOrder":0,"isDefault":true,"isSubscribed":true,"shareWith":null,"myRights":{"mayRead":true,"mayWrite":true,"mayShare":false,"mayDelete":false}}],"notFound":[]})");
    REQUIRE(books.ok());
    REQUIRE(books.value->list.size() == 1);
    CHECK(books.value->list.front().isDefault);
    CHECK(books.value->list.front().myRights.mayWrite);

    const auto cards = javelin::jmap::api::parseContactCardGetResponse(
        R"({"accountId":"a0x9","state":"c1","list":[{"id":"3","uid":"urn:uuid:3","kind":"individual","addressBookIds":{"book-1":true},"name":{"full":"Joe Bloggs"},"x-company-extension":{"value":42}}],"notFound":[]})");
    REQUIRE(cards.ok());
    REQUIRE(cards.value->list.size() == 1);
    CHECK(cards.value->list.front().uid == "urn:uuid:3");
    CHECK(cards.value->list.front().document.find("x-company-extension") != std::string::npos);
}

TEST_CASE("contacts query supports RFC 9610 filter properties", "[jmap][contacts][protocol]")
{
    javelin::jmap::api::ContactCardQueryFilter filter;
    filter.inAddressBook = "book-1";
    filter.nameGiven = "Joe";
    filter.nameSurname = "Bloggs";
    filter.nameSurname2 = "Smith";
    filter.onlineService = "example";
    javelin::jmap::api::ContactCardQueryRequest request;
    request.accountId = "a0x9";
    request.filter = std::move(filter);
    request.sort.push_back(
        {.property = "name/surname", .isAscending = true, .collation = std::nullopt});
    request.calculateTotal = true;
    const auto json = javelin::jmap::api::serializeContactCardQueryRequest(request);
    REQUIRE(json.has_value());
    CHECK(json->find(R"("name/given":"Joe")") != std::string::npos);
    CHECK(json->find(R"("name/surname2":"Smith")") != std::string::npos);
    CHECK(json->find(R"("onlineService":"example")") != std::string::npos);
}

TEST_CASE("contacts set and copy embed JSContact documents", "[jmap][contacts][protocol]")
{
    javelin::jmap::api::ContactCardSetRequest setRequest;
    setRequest.accountId = "a0x9";
    setRequest.create.emplace("new-1",
                              javelin::jmap::api::ContactDocument{
                                  .json = R"({"uid":"u1","kind":"group","members":{"u2":true}})"});
    const auto setJson = javelin::jmap::api::serializeContactCardSetRequest(setRequest);
    REQUIRE(setJson.has_value());
    CHECK(setJson->find(R"("new-1":{"uid":"u1")") != std::string::npos);
    CHECK(setJson->find(R"("new-1":"{")") == std::string::npos);

    javelin::jmap::api::ContactCardCopyRequest copyRequest;
    copyRequest.fromAccountId = "source";
    copyRequest.accountId = "destination";
    copyRequest.create.emplace("copy-1",
                               javelin::jmap::api::ContactDocument{.json = R"({"id":"card-1"})"});
    copyRequest.onSuccessDestroyOriginal = true;
    const auto copyJson = javelin::jmap::api::serializeContactCardCopyRequest(copyRequest);
    REQUIRE(copyJson.has_value());
    CHECK(copyJson->find(R"("onSuccessDestroyOriginal":true)") != std::string::npos);
}

TEST_CASE("contacts set responses preserve patches and errors", "[jmap][contacts][protocol]")
{
    const auto response = javelin::jmap::api::parseContactsSetResponse(
        R"({"accountId":"a0x9","oldState":"1","newState":"2","created":{"c1":{"id":"card-1"}},"updated":{"card-2":{"updated":"2026-07-10T00:00:00Z"}},"destroyed":["card-3"],"notCreated":{},"notUpdated":{},"notDestroyed":{"book-1":{"type":"addressBookHasContents","description":"Not empty"}}})");
    REQUIRE(response.ok());
    CHECK(response.value->created.at("c1").json.find("card-1") != std::string::npos);
    CHECK(response.value->notDestroyed.at("book-1").json.find("addressBookHasContents") !=
          std::string::npos);
}

TEST_CASE("contact document editing removes immutable ids and retains extensions",
          "[jmap][contacts][document]")
{
    const auto prepared = javelin::jmap::contacts::prepareContactDocument(
        R"({"id":"server-id","uid":"u1","kind":"individual","addressBookIds":{"b1":true},"x-extension":{"answer":42}})",
        false);
    REQUIRE(std::holds_alternative<std::string>(prepared));
    const auto& json = std::get<std::string>(prepared);
    CHECK(json.find("server-id") == std::string::npos);
    CHECK(json.find("x-extension") != std::string::npos);

    const auto photo = javelin::jmap::contacts::setContactPhoto(json, "blob-1", "image/png");
    REQUIRE(std::holds_alternative<std::string>(photo));
    CHECK(std::get<std::string>(photo).find("blob-1") != std::string::npos);

    const auto editor = javelin::jmap::contacts::contactEditorData(json);
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactEditorData>(editor));
    auto fields = std::get<javelin::jmap::contacts::ContactEditorData>(editor);
    fields.fullName = "Joe Bloggs";
    fields.organization = "Example Ltd";
    fields.emails = {{.key = {},
                      .value = "joe@example.test",
                      .label = std::nullopt,
                      .preference = std::nullopt,
                      .contexts = {}}};
    fields.phones = {{.key = {},
                      .value = "+64 21 555 0100",
                      .label = std::nullopt,
                      .preference = std::nullopt,
                      .contexts = {}}};
    fields.addresses = {{.key = {},
                         .value = "1 Example Street, Auckland",
                         .label = std::nullopt,
                         .preference = std::nullopt,
                         .contexts = {}}};
    fields.birthday = "--04-15";
    fields.notes = "Met at the JMAP conference.";
    const auto edited = javelin::jmap::contacts::applyContactEditorData(fields, false);
    REQUIRE(std::holds_alternative<std::string>(edited));
    CHECK(std::get<std::string>(edited).find("Joe Bloggs") != std::string::npos);
    CHECK(std::get<std::string>(edited).find("x-extension") != std::string::npos);
    CHECK(std::get<std::string>(edited).find("javelin-birthday") != std::string::npos);
    const auto reparsed = javelin::jmap::contacts::contactEditorData(std::get<std::string>(edited));
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactEditorData>(reparsed));
    CHECK(std::get<javelin::jmap::contacts::ContactEditorData>(reparsed).birthday == "--04-15");
}

TEST_CASE("structured contact fields and unresolved group members survive editing",
          "[jmap][contacts][document]")
{
    const std::string document =
        R"({"id":"g1","uid":"group-uid","kind":"group","addressBookIds":{"b1":true},"name":{"full":"Friends"},"emails":{"email-work":{"address":"old@example.test","label":"Office","pref":7,"contexts":{"work":true},"x-field":"preserved"}},"phones":{"phone-home":{"number":"123","contexts":{"private":true,"voice":true}}},"addresses":{"address-home":{"components":[{"kind":"street","value":"1 Example Street"},{"kind":"locality","value":"Auckland"}],"label":"Home"}},"members":{"known-uid":true,"temporarily-unavailable-uid":true},"x-card":"preserved"})";
    const auto parsed = javelin::jmap::contacts::contactEditorData(document);
    REQUIRE(std::holds_alternative<javelin::jmap::contacts::ContactEditorData>(parsed));
    auto fields = std::get<javelin::jmap::contacts::ContactEditorData>(parsed);
    REQUIRE(fields.emails.size() == 1);
    CHECK(fields.emails.front().key == "email-work");
    CHECK(fields.emails.front().label == std::optional<std::string>{"Office"});
    CHECK(fields.emails.front().preference == std::optional<std::uint32_t>{7});
    CHECK(fields.emails.front().contexts.at("work"));
    REQUIRE(fields.addresses.size() == 1);
    CHECK(fields.addresses.front().value == "1 Example Street, Auckland");
    REQUIRE(fields.members.size() == 2);

    fields.emails.front().value = "new@example.test";
    fields.emails.front().preference = 1;
    fields.members.erase(fields.members.begin());
    const auto edited = javelin::jmap::contacts::applyContactEditorData(fields, false);
    REQUIRE(std::holds_alternative<std::string>(edited));
    const auto& json = std::get<std::string>(edited);
    CHECK(json.find("new@example.test") != std::string::npos);
    CHECK(json.find(R"("pref":1)") != std::string::npos);
    CHECK(json.find("x-field") != std::string::npos);
    CHECK(json.find("x-card") != std::string::npos);
    CHECK(json.find("temporarily-unavailable-uid") != std::string::npos);
    CHECK(json.find("known-uid") == std::string::npos);
}

TEST_CASE("contact action rights require every membership to be writable",
          "[jmap][contacts][rights]")
{
    const std::vector books{
        javelin::jmap::api::AddressBook{
            .id = "writable",
            .name = "Writable",
            .description = std::nullopt,
            .sortOrder = 0,
            .isDefault = true,
            .isSubscribed = true,
            .shareWith = std::nullopt,
            .myRights = {.mayRead = true, .mayWrite = true, .mayShare = false, .mayDelete = false}},
        javelin::jmap::api::AddressBook{.id = "shared-read-only",
                                        .name = "Shared",
                                        .description = std::nullopt,
                                        .sortOrder = 1,
                                        .isDefault = false,
                                        .isSubscribed = true,
                                        .shareWith = std::nullopt,
                                        .myRights = {.mayRead = true,
                                                     .mayWrite = false,
                                                     .mayShare = false,
                                                     .mayDelete = false}},
    };
    const std::vector writableMembership{std::string{"writable"}};
    const auto writable =
        javelin::jmap::contacts::contactActionRights(false, books, writableMembership);
    CHECK(writable.mayCreate);
    CHECK(writable.mayModify);
    CHECK(writable.mayDestroy);

    const std::vector mixedMembership{std::string{"writable"}, std::string{"shared-read-only"}};
    const auto mixed = javelin::jmap::contacts::contactActionRights(false, books, mixedMembership);
    CHECK(mixed.mayCreate);
    CHECK_FALSE(mixed.mayModify);
    CHECK_FALSE(mixed.mayDestroy);

    const auto readOnlyAccount =
        javelin::jmap::contacts::contactActionRights(true, books, writableMembership);
    CHECK_FALSE(readOnlyAccount.mayCreate);
    CHECK_FALSE(readOnlyAccount.mayModify);
    CHECK_FALSE(readOnlyAccount.mayDestroy);
}

TEST_CASE("contact starring preserves document extensions and removes legacy importance",
          "[jmap][contacts][document]")
{
    const auto starred = javelin::jmap::contacts::setContactStarred(
        R"({"id":"server-id","uid":"u1","kind":"individual","keywords":{"important":true,"other":true},"x-extension":{"answer":42}})",
        true);
    REQUIRE(std::holds_alternative<std::string>(starred));
    const auto& starredJson = std::get<std::string>(starred);
    CHECK(starredJson.find("server-id") == std::string::npos);
    CHECK(starredJson.find(R"("starred":true)") != std::string::npos);
    CHECK(starredJson.find("x-extension") != std::string::npos);

    const auto unstarred = javelin::jmap::contacts::setContactStarred(starredJson, false);
    REQUIRE(std::holds_alternative<std::string>(unstarred));
    const auto& unstarredJson = std::get<std::string>(unstarred);
    CHECK(unstarredJson.find("starred") == std::string::npos);
    CHECK(unstarredJson.find("important") == std::string::npos);
    CHECK(unstarredJson.find(R"("other":true)") != std::string::npos);
}
