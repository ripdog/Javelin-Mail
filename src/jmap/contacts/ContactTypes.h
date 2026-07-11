#pragma once

#include "jmap/api/ContactsMethods.h"

#include <optional>
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
        std::string document;
    };

    struct ContactEditorData
    {
        std::string kind;
        std::string fullName;
        std::string organization;
        std::string title;
        std::vector<std::string> emails;
        std::vector<std::string> phones;
        std::vector<std::string> addresses;
        std::string birthday;
        std::string notes;
        std::vector<std::string> addressBookIds;
        std::string document;
    };

    [[nodiscard]] std::optional<ContactSummary>
    summarizeContact(std::string accountId, const javelin::jmap::api::ContactCard& card);
    [[nodiscard]] std::string normalizeEmail(std::string_view email);
    [[nodiscard]] std::variant<std::string, std::string_view>
    prepareContactDocument(std::string_view json, bool creating);
    [[nodiscard]] std::variant<std::string, std::string_view>
    setContactPhoto(std::string_view json, std::string blobId, std::string mediaType);
    [[nodiscard]] std::variant<ContactEditorData, std::string_view>
    contactEditorData(std::string_view json);
    [[nodiscard]] std::variant<std::string, std::string_view>
    applyContactEditorData(const ContactEditorData& data, bool creating);
} // namespace javelin::jmap::contacts
