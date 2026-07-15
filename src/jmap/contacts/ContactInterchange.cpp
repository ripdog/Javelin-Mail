#include "jmap/contacts/ContactInterchange.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace javelin::jmap::contacts
{
    namespace
    {
        [[nodiscard]] std::string normalizedPhone(const std::string_view value)
        {
            std::string result;
            for (const char character : value)
            {
                if (std::isdigit(static_cast<unsigned char>(character)) != 0)
                    result.push_back(character);
            }
            return result.size() >= 7 ? result : std::string{};
        }

        [[nodiscard]] std::string escapedVCard(std::string value)
        {
            std::string result;
            result.reserve(value.size());
            for (const char character : value)
            {
                if (character == '\\' || character == ',' || character == ';')
                    result.push_back('\\');
                if (character == '\n')
                    result += "\\n";
                else if (character != '\r')
                    result.push_back(character);
            }
            return result;
        }

        [[nodiscard]] std::string unescapedVCard(const std::string_view value)
        {
            std::string result;
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                if (value[index] != '\\' || index + 1 >= value.size())
                {
                    result.push_back(value[index]);
                    continue;
                }
                const char escaped = value[++index];
                result.push_back(escaped == 'n' || escaped == 'N' ? '\n' : escaped);
            }
            return result;
        }

        [[nodiscard]] std::string contextParameter(const ContactEditorField& field)
        {
            if (const auto home = field.contexts.find("private");
                home != field.contexts.end() && home->second)
                return ";TYPE=home";
            if (const auto work = field.contexts.find("work");
                work != field.contexts.end() && work->second)
                return ";TYPE=work";
            return {};
        }

        [[nodiscard]] std::string foldedVCard(const std::string_view input)
        {
            std::string result;
            std::size_t lineStart = 0;
            while (lineStart < input.size())
            {
                const auto lineEnd = input.find("\r\n", lineStart);
                const auto end = lineEnd == std::string_view::npos ? input.size() : lineEnd;
                std::string_view line = input.substr(lineStart, end - lineStart);
                std::size_t limit = 75;
                while (line.size() > limit)
                {
                    std::size_t split = limit;
                    while (split > 0 && (static_cast<unsigned char>(line[split]) & 0xc0U) == 0x80U)
                        --split;
                    if (split == 0)
                        split = limit;
                    result.append(line.substr(0, split));
                    result += "\r\n ";
                    line.remove_prefix(split);
                    limit = 74;
                }
                result.append(line);
                result += "\r\n";
                if (lineEnd == std::string_view::npos)
                    break;
                lineStart = lineEnd + 2;
            }
            return result;
        }

        void appendField(std::ostringstream& output, const std::string_view property,
                         const ContactEditorField& field)
        {
            output << property << contextParameter(field);
            if (field.preference.has_value())
                output << ";PREF=" << *field.preference;
            if (field.label.has_value() && !field.label->empty())
                output << ";LABEL=" << escapedVCard(*field.label);
            output << ':' << escapedVCard(field.value) << "\r\n";
        }

        [[nodiscard]] std::unordered_map<std::string, std::string>
        parameters(const std::string_view prefix)
        {
            std::unordered_map<std::string, std::string> result;
            std::size_t start = prefix.find(';');
            while (start != std::string_view::npos)
            {
                ++start;
                const auto end = prefix.find(';', start);
                const auto parameter = prefix.substr(start, end - start);
                const auto equals = parameter.find('=');
                if (equals != std::string_view::npos)
                {
                    std::string key{parameter.substr(0, equals)};
                    std::ranges::transform(key, key.begin(), [](const unsigned char character)
                                           { return static_cast<char>(std::toupper(character)); });
                    result.emplace(std::move(key), std::string{parameter.substr(equals + 1)});
                }
                start = end;
            }
            return result;
        }

        [[nodiscard]] ContactEditorField parsedField(const std::string_view prefix,
                                                     const std::string_view value)
        {
            const auto values = parameters(prefix);
            ContactEditorField field{.key = {},
                                     .value = unescapedVCard(value),
                                     .label = std::nullopt,
                                     .preference = std::nullopt,
                                     .contexts = {}};
            if (const auto label = values.find("LABEL"); label != values.end())
                field.label = unescapedVCard(label->second);
            if (const auto pref = values.find("PREF"); pref != values.end())
            {
                std::uint32_t valueNumber = 0;
                const auto [end, error] = std::from_chars(
                    pref->second.data(), pref->second.data() + pref->second.size(), valueNumber);
                if (error == std::errc{} && end == pref->second.data() + pref->second.size() &&
                    valueNumber > 0)
                    field.preference = valueNumber;
            }
            if (const auto type = values.find("TYPE"); type != values.end())
            {
                std::string normalized = type->second;
                std::ranges::transform(normalized, normalized.begin(),
                                       [](const unsigned char character)
                                       { return static_cast<char>(std::tolower(character)); });
                if (normalized.find("home") != std::string::npos)
                    field.contexts.emplace("private", true);
                if (normalized.find("work") != std::string::npos)
                    field.contexts.emplace("work", true);
            }
            return field;
        }
    } // namespace

    std::vector<DuplicateContactGroup>
    findDuplicateContacts(const std::span<const ContactSummary> contacts)
    {
        std::vector<std::size_t> parents(contacts.size());
        std::iota(parents.begin(), parents.end(), 0);
        const auto find = [&parents](std::size_t index)
        {
            while (parents[index] != index)
            {
                parents[index] = parents[parents[index]];
                index = parents[index];
            }
            return index;
        };
        const auto unite = [&parents, &find](const std::size_t left, const std::size_t right)
        {
            const auto leftRoot = find(left);
            const auto rightRoot = find(right);
            if (leftRoot != rightRoot)
                parents[rightRoot] = leftRoot;
        };
        std::unordered_map<std::string, std::size_t> identities;
        for (std::size_t index = 0; index < contacts.size(); ++index)
        {
            for (const auto& email : contacts[index].emails)
            {
                const auto key = contacts[index].kind + ":email:" + normalizeEmail(email.address);
                if (key.ends_with("email:"))
                    continue;
                const auto [found, inserted] = identities.emplace(key, index);
                if (!inserted)
                    unite(index, found->second);
            }
            const auto editor = contactEditorData(contacts[index].document);
            if (const auto* data = std::get_if<ContactEditorData>(&editor))
            {
                for (const auto& phone : data->phones)
                {
                    const auto number = normalizedPhone(phone.value);
                    if (number.empty())
                        continue;
                    const auto [found, inserted] =
                        identities.emplace(contacts[index].kind + ":phone:" + number, index);
                    if (!inserted)
                        unite(index, found->second);
                }
            }
        }
        std::unordered_map<std::size_t, std::vector<std::string>> grouped;
        for (std::size_t index = 0; index < contacts.size(); ++index)
            grouped[find(index)].push_back(contacts[index].id);
        std::vector<DuplicateContactGroup> result;
        for (auto& [root, ids] : grouped)
        {
            static_cast<void>(root);
            if (ids.size() > 1)
                result.push_back({.contactIds = std::move(ids)});
        }
        return result;
    }

    std::variant<std::string, std::string_view>
    mergeContactDocuments(const std::string_view primaryJson, const std::string_view duplicateJson)
    {
        std::string primaryBuffer{primaryJson};
        std::string duplicateBuffer{duplicateJson};
        glz::generic primary;
        glz::generic duplicate;
        if (glz::read_json(primary, primaryBuffer) || glz::read_json(duplicate, duplicateBuffer) ||
            !primary.is_object() || !duplicate.is_object())
            return std::string_view{"Both contacts must be valid JSON objects."};
        primary.get_object().erase("id");
        constexpr std::array collectionProperties{
            "addressBookIds", "keywords",       "emails",  "phones",   "addresses",
            "organizations",  "titles",         "notes",   "media",    "anniversaries",
            "personalInfo",   "onlineServices", "members", "relatedTo"};
        constexpr std::array setProperties{"addressBookIds", "keywords", "members"};
        for (const auto property : collectionProperties)
        {
            if (!duplicate.contains(property) || !duplicate.at(property).is_object())
                continue;
            if (!primary.contains(property))
            {
                primary[property] = duplicate.at(property);
                continue;
            }
            if (!primary.at(property).is_object())
                continue;
            auto& destination = primary.at(property).get_object();
            for (const auto& [sourceKey, sourceValue] : duplicate.at(property).get_object())
            {
                std::string key = sourceKey;
                if (std::ranges::find(setProperties, property) != setProperties.end() &&
                    destination.contains(key))
                    continue;
                std::size_t suffix = 2;
                while (destination.contains(key))
                    key = sourceKey + "-merged-" + std::to_string(suffix++);
                destination.emplace(std::move(key), sourceValue);
            }
        }
        for (const auto& [key, value] : duplicate.get_object())
        {
            if (key == "id" || key == "uid" || key == "kind" || key == "name" ||
                std::ranges::find(collectionProperties, key) != collectionProperties.end())
                continue;
            if (!primary.contains(key))
                primary[key] = value;
        }
        std::string result;
        if (glz::write_json(primary, result))
            return std::string_view{"Unable to serialize the merged contact."};
        return result;
    }

    std::string exportVCard(const ContactEditorData& contact)
    {
        std::ostringstream output;
        output << "BEGIN:VCARD\r\nVERSION:4.0\r\n";
        if (!contact.uid.empty())
            output << "UID:" << escapedVCard(contact.uid) << "\r\n";
        output << "FN:" << escapedVCard(contact.fullName) << "\r\n";
        if (!contact.kind.empty())
            output << "KIND:" << (contact.kind == "org" ? "org" : contact.kind) << "\r\n";
        if (!contact.organization.empty())
            output << "ORG:" << escapedVCard(contact.organization) << "\r\n";
        if (!contact.title.empty())
            output << "TITLE:" << escapedVCard(contact.title) << "\r\n";
        for (const auto& email : contact.emails)
            appendField(output, "EMAIL", email);
        for (const auto& phone : contact.phones)
            appendField(output, "TEL", phone);
        for (const auto& address : contact.addresses)
        {
            output << "ADR" << contextParameter(address);
            if (address.preference.has_value())
                output << ";PREF=" << *address.preference;
            output << ";LABEL=" << escapedVCard(address.value);
            if (address.label.has_value() && !address.label->empty())
                output << ";X-JAVELIN-LABEL=" << escapedVCard(*address.label);
            output << ":;;;;;;\r\n";
        }
        for (const auto& member : contact.members)
            output << "MEMBER:urn:uuid:" << escapedVCard(member) << "\r\n";
        if (!contact.birthday.empty())
            output << "BDAY:" << contact.birthday << "\r\n";
        if (!contact.notes.empty())
            output << "NOTE:" << escapedVCard(contact.notes) << "\r\n";
        output << "END:VCARD\r\n";
        return foldedVCard(output.str());
    }

    std::variant<std::vector<ContactEditorData>, std::string_view>
    importVCards(const std::string_view text)
    {
        std::vector<std::string> lines;
        std::istringstream input{std::string{text}};
        for (std::string line; std::getline(input, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!lines.empty() && !line.empty() && (line.front() == ' ' || line.front() == '\t'))
                lines.back() += line.substr(1);
            else
                lines.push_back(std::move(line));
        }
        std::vector<ContactEditorData> result;
        std::optional<ContactEditorData> current;
        for (const auto& line : lines)
        {
            if (line == "BEGIN:VCARD")
            {
                if (current.has_value())
                    return std::string_view{"Nested vCards are not valid."};
                current.emplace();
                continue;
            }
            if (line == "END:VCARD")
            {
                if (!current.has_value() || current->fullName.empty())
                    return std::string_view{"Every imported vCard requires an FN property."};
                result.push_back(std::move(*current));
                current.reset();
                continue;
            }
            if (!current.has_value())
                continue;
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const auto prefix = std::string_view{line}.substr(0, colon);
            const auto value = std::string_view{line}.substr(colon + 1);
            std::string property{prefix.substr(0, prefix.find(';'))};
            std::ranges::transform(property, property.begin(), [](const unsigned char character)
                                   { return static_cast<char>(std::toupper(character)); });
            if (property == "UID")
                current->uid = unescapedVCard(value);
            else if (property == "FN")
                current->fullName = unescapedVCard(value);
            else if (property == "KIND")
                current->kind = value == "org" ? "org" : std::string{value};
            else if (property == "ORG")
                current->organization = unescapedVCard(value);
            else if (property == "TITLE")
                current->title = unescapedVCard(value);
            else if (property == "EMAIL")
                current->emails.push_back(parsedField(prefix, value));
            else if (property == "TEL")
                current->phones.push_back(parsedField(prefix, value));
            else if (property == "ADR")
            {
                auto field = parsedField(prefix, value);
                const auto values = parameters(prefix);
                if (const auto label = values.find("LABEL"); label != values.end())
                    field.value = unescapedVCard(label->second);
                if (const auto label = values.find("X-JAVELIN-LABEL"); label != values.end())
                    field.label = unescapedVCard(label->second);
                else
                    field.label.reset();
                current->addresses.push_back(std::move(field));
            }
            else if (property == "MEMBER")
            {
                constexpr std::string_view prefixValue{"urn:uuid:"};
                current->members.push_back(unescapedVCard(
                    value.starts_with(prefixValue) ? value.substr(prefixValue.size()) : value));
            }
            else if (property == "BDAY")
                current->birthday = std::string{value};
            else if (property == "NOTE")
                current->notes = unescapedVCard(value);
        }
        if (current.has_value())
            return std::string_view{"The final vCard is missing END:VCARD."};
        if (result.empty())
            return std::string_view{"The file does not contain a vCard."};
        for (auto& contact : result)
        {
            if (contact.kind.empty())
                contact.kind = "individual";
        }
        return result;
    }

    std::variant<std::string, std::string_view> importedContactDocument(ContactEditorData contact,
                                                                        std::string addressBookId,
                                                                        std::string fallbackUid)
    {
        if (contact.uid.empty())
            contact.uid = std::move(fallbackUid);
        contact.addressBookIds = {std::move(addressBookId)};
        glz::generic document;
        document["uid"] = contact.uid;
        document["kind"] = contact.kind.empty() ? std::string{"individual"} : contact.kind;
        document["addressBookIds"][contact.addressBookIds.front()] = true;
        document["name"]["full"] = contact.fullName;
        if (glz::write_json(document, contact.document))
            return std::string_view{"Unable to prepare the imported contact."};
        return applyContactEditorData(contact, true);
    }
} // namespace javelin::jmap::contacts
