#pragma once

#include "jmap/contacts/ContactTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace javelin::jmap::contacts
{
    struct DuplicateContactGroup
    {
        std::vector<std::string> contactIds;
    };

    [[nodiscard]] std::vector<DuplicateContactGroup>
    findDuplicateContacts(std::span<const ContactSummary> contacts);
    [[nodiscard]] std::variant<std::string, std::string_view>
    mergeContactDocuments(std::string_view primaryJson, std::string_view duplicateJson);
    [[nodiscard]] std::string exportVCard(const ContactEditorData& contact);
    [[nodiscard]] std::variant<std::vector<ContactEditorData>, std::string_view>
    importVCards(std::string_view text);
    [[nodiscard]] std::variant<std::string, std::string_view>
    importedContactDocument(ContactEditorData contact, std::string addressBookId,
                            std::string fallbackUid);
} // namespace javelin::jmap::contacts
