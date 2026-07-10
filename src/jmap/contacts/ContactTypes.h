#pragma once

#include "jmap/api/ContactsMethods.h"

#include <optional>
#include <string>
#include <unordered_map>
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

    [[nodiscard]] std::optional<ContactSummary>
    summarizeContact(std::string accountId, const javelin::jmap::api::ContactCard& card);
    [[nodiscard]] std::string normalizeEmail(std::string_view email);
} // namespace javelin::jmap::contacts
