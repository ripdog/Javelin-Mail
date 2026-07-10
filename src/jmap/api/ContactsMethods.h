#pragma once

#include "jmap/api/MailMethods.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace javelin::jmap::api
{
    struct AddressBookRights
    {
        bool mayRead = false;
        bool mayWrite = false;
        bool mayShare = false;
        bool mayDelete = false;
    };

    struct AddressBook
    {
        std::string id;
        std::string name;
        std::optional<std::string> description;
        std::uint32_t sortOrder = 0;
        bool isDefault = false;
        bool isSubscribed = false;
        std::optional<std::unordered_map<std::string, AddressBookRights>> shareWith;
        AddressBookRights myRights;
    };

    struct ContactCard
    {
        std::string id;
        std::string uid;
        std::string kind;
        std::string document;
    };

    struct AddressBookGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<AddressBook> list;
        std::vector<std::string> notFound;
    };

    struct ContactCardGetResponse
    {
        std::string accountId;
        std::string state;
        std::vector<ContactCard> list;
        std::vector<std::string> notFound;
    };

    struct ContactCardQueryFilter
    {
        std::optional<std::string> operatorName;
        std::vector<ContactCardQueryFilter> conditions;
        std::optional<std::string> inAddressBook;
        std::optional<std::string> uid;
        std::optional<std::string> hasMember;
        std::optional<std::string> kind;
        std::optional<std::string> createdBefore;
        std::optional<std::string> createdAfter;
        std::optional<std::string> updatedBefore;
        std::optional<std::string> updatedAfter;
        std::optional<std::string> text;
        std::optional<std::string> name;
        std::optional<std::string> nameGiven;
        std::optional<std::string> nameSurname;
        std::optional<std::string> nameSurname2;
        std::optional<std::string> nickname;
        std::optional<std::string> organization;
        std::optional<std::string> email;
        std::optional<std::string> phone;
        std::optional<std::string> onlineService;
        std::optional<std::string> address;
        std::optional<std::string> note;
    };

    struct ContactCardComparator
    {
        std::string property;
        bool isAscending = true;
        std::optional<std::string> collation;
    };

    struct ContactCardQueryRequest
    {
        std::string accountId;
        std::optional<ContactCardQueryFilter> filter;
        std::vector<ContactCardComparator> sort;
        std::optional<std::uint64_t> position;
        std::optional<std::string> anchor;
        std::int64_t anchorOffset = 0;
        std::optional<std::uint64_t> limit;
        bool calculateTotal = false;
    };

    struct ContactCardQueryResponse
    {
        std::string accountId;
        std::string queryState;
        bool canCalculateChanges = false;
        std::uint64_t position = 0;
        std::vector<std::string> ids;
        std::optional<std::uint64_t> total;
        std::optional<std::uint64_t> limit;
    };

    struct ContactCardQueryChangesRequest
    {
        std::string accountId;
        std::string sinceQueryState;
        std::optional<std::uint64_t> maxChanges;
        std::optional<std::string> upToId;
        std::optional<ContactCardQueryFilter> filter;
        std::vector<ContactCardComparator> sort;
        bool calculateTotal = false;
    };

    using ContactCardQueryChangesResponse = EmailQueryChangesResponse;

    struct ContactDocument
    {
        std::string json;
    };

    struct AddressBookSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, ContactDocument> create;
        std::unordered_map<std::string, ContactDocument> update;
        std::vector<std::string> destroy;
        bool onDestroyRemoveContents = false;
        std::optional<std::string> onSuccessSetIsDefault;
    };

    struct ContactCardSetRequest
    {
        std::string accountId;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, ContactDocument> create;
        std::unordered_map<std::string, ContactDocument> update;
        std::vector<std::string> destroy;
    };

    struct SetResult
    {
        std::string accountId;
        std::string oldState;
        std::string newState;
        std::unordered_map<std::string, ContactDocument> created;
        std::unordered_map<std::string, ContactDocument> updated;
        std::vector<std::string> destroyed;
        std::unordered_map<std::string, ContactDocument> notCreated;
        std::unordered_map<std::string, ContactDocument> notUpdated;
        std::unordered_map<std::string, ContactDocument> notDestroyed;
    };

    struct ContactCardCopyRequest
    {
        std::string fromAccountId;
        std::string accountId;
        std::optional<std::string> ifFromInState;
        std::optional<std::string> ifInState;
        std::unordered_map<std::string, ContactDocument> create;
        bool onSuccessDestroyOriginal = false;
    };

    using ContactCardCopyResponse = SetResult;

    [[nodiscard]] std::optional<std::string>
    serializeContactCardQueryRequest(const ContactCardQueryRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeContactCardQueryChangesRequest(const ContactCardQueryChangesRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeAddressBookSetRequest(const AddressBookSetRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeContactCardSetRequest(const ContactCardSetRequest& request);
    [[nodiscard]] std::optional<std::string>
    serializeContactCardCopyRequest(const ContactCardCopyRequest& request);

    [[nodiscard]] ParsedEnvelope<AddressBookGetResponse>
    parseAddressBookGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ContactCardGetResponse>
    parseContactCardGetResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ContactCardQueryResponse>
    parseContactCardQueryResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<ContactCardQueryChangesResponse>
    parseContactCardQueryChangesResponse(std::string_view json);
    [[nodiscard]] ParsedEnvelope<SetResult> parseContactsSetResponse(std::string_view json);
} // namespace javelin::jmap::api
