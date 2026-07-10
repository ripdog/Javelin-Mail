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
}
