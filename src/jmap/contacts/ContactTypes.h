#pragma once

#include "jmap/api/ContactsMethods.h"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace javelin::jmap::contacts
{
    struct ContactEmail
    {
        std::string key;
        std::string address;
        std::optional<std::string> label;
        std::optional<std::uint32_t> preference;
        std::unordered_map<std::string, bool> contexts;
    };

    struct ContactEditorField
    {
        std::string key;
        std::string value;
        std::optional<std::string> label;
        std::optional<std::uint32_t> preference;
        std::unordered_map<std::string, bool> contexts;
    };

    struct ContactSummary
    {
        std::string accountId;
        std::string id;
        std::string uid;
        std::string kind;
        std::string displayName;
        std::optional<std::string> organization;
        std::vector<ContactEmail> emails;
        std::vector<std::string> addressBookIds;
        bool isImportant = false;
        std::string document;
    };

    struct ContactEditorData
    {
        std::string uid;
        std::string kind;
        std::string fullName;
        std::string organization;
        std::string title;
        std::vector<ContactEditorField> emails;
        std::vector<ContactEditorField> phones;
        std::vector<ContactEditorField> addresses;
        std::vector<std::string> members;
        std::string birthday;
        std::string notes;
        std::vector<std::string> addressBookIds;
        std::string document;
    };

    struct ContactActionRights
    {
        bool mayCreate = false;
        bool mayModify = false;
        bool mayDestroy = false;
    };

    struct ContactPhoto
    {
        std::string key;
        std::optional<std::string> blobId;
        std::optional<std::string> uri;
        std::optional<std::string> mediaType;
    };

    [[nodiscard]] std::optional<ContactSummary>
    summarizeContact(std::string accountId, const javelin::jmap::api::ContactCard& card);
    [[nodiscard]] std::string normalizeEmail(std::string_view email);
    [[nodiscard]] std::variant<std::string, std::string_view>
    prepareContactDocument(std::string_view json, bool creating);
    [[nodiscard]] std::variant<std::string, std::string_view>
    setContactPhoto(std::string_view json, std::string blobId, std::string mediaType);
    [[nodiscard]] std::optional<ContactPhoto> contactPhoto(std::string_view json);
    [[nodiscard]] std::variant<std::string, std::string_view>
    removeContactPhoto(std::string_view json);
    [[nodiscard]] std::variant<std::string, std::string_view>
    setContactStarred(std::string_view json, bool starred);
    [[nodiscard]] std::variant<ContactEditorData, std::string_view>
    contactEditorData(std::string_view json);
    [[nodiscard]] std::variant<std::string, std::string_view>
    applyContactEditorData(const ContactEditorData& data, bool creating);
    [[nodiscard]] std::variant<std::string, std::string_view>
    createContactGroupDocument(std::string name, std::string uid, std::string addressBookId);
    [[nodiscard]] std::variant<std::string, std::string_view>
    contactGroupMembershipPatch(std::string_view memberUid, bool included);
    [[nodiscard]] std::variant<std::string, std::string_view>
    contactGroupMembershipPatch(std::span<const std::string> memberUids, bool included);
    [[nodiscard]] ContactActionRights
    contactActionRights(bool accountReadOnly,
                        std::span<const javelin::jmap::api::AddressBook> addressBooks,
                        std::span<const std::string> contactAddressBookIds = {});
} // namespace javelin::jmap::contacts
