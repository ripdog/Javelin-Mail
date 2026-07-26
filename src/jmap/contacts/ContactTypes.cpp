#include "jmap/contacts/ContactTypes.h"

#include "jmap/api/PatchObject.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <span>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace javelin::jmap::contacts::detail
{
    struct NameComponent
    {
        std::string kind;
        std::string value;
    };
    struct Name
    {
        std::optional<std::string> full;
        std::vector<NameComponent> components;
    };
    struct Email
    {
        std::string address;
        std::optional<std::string> label;
        std::optional<std::uint32_t> pref;
        std::unordered_map<std::string, bool> contexts;
    };
    struct Organization
    {
        std::string name;
    };
    struct Card
    {
        std::string id;
        std::string uid;
        std::string kind;
        std::unordered_map<std::string, bool> addressBookIds;
        std::unordered_map<std::string, bool> keywords;
        std::optional<Name> name;
        std::unordered_map<std::string, Email> emails;
        std::unordered_map<std::string, Organization> organizations;
    };
} // namespace javelin::jmap::contacts::detail

template <> struct glz::meta<javelin::jmap::contacts::detail::NameComponent>
{
    using T = javelin::jmap::contacts::detail::NameComponent;
    static constexpr auto value = glz::object("kind", &T::kind, "value", &T::value);
};
template <> struct glz::meta<javelin::jmap::contacts::detail::Name>
{
    using T = javelin::jmap::contacts::detail::Name;
    static constexpr auto value = glz::object("full", &T::full, "components", &T::components);
};
template <> struct glz::meta<javelin::jmap::contacts::detail::Email>
{
    using T = javelin::jmap::contacts::detail::Email;
    static constexpr auto value = glz::object("address", &T::address, "label", &T::label, "pref",
                                              &T::pref, "contexts", &T::contexts);
};
template <> struct glz::meta<javelin::jmap::contacts::detail::Organization>
{
    using T = javelin::jmap::contacts::detail::Organization;
    static constexpr auto value = glz::object("name", &T::name);
};
template <> struct glz::meta<javelin::jmap::contacts::detail::Card>
{
    using T = javelin::jmap::contacts::detail::Card;
    static constexpr auto value =
        glz::object("id", &T::id, "uid", &T::uid, "kind", &T::kind, "addressBookIds",
                    &T::addressBookIds, "keywords", &T::keywords, "name", &T::name, "emails",
                    &T::emails, "organizations", &T::organizations);
};

namespace javelin::jmap::contacts
{
    namespace
    {
        [[nodiscard]] std::string stringProperty(const glz::generic& object,
                                                 const std::string_view key)
        {
            if (!object.is_object() || !object.contains(key) || !object.at(key).is_string())
                return {};
            return object.at(key).get_string();
        }

        [[nodiscard]] std::vector<std::string> mappedStrings(const glz::generic& root,
                                                             const std::string_view mapName,
                                                             const std::string_view property)
        {
            std::vector<std::string> result;
            if (!root.is_object() || !root.contains(mapName) || !root.at(mapName).is_object())
                return result;
            for (const auto& entry : root.at(mapName).get_object())
            {
                const auto text = stringProperty(entry.second, property);
                if (!text.empty())
                    result.push_back(text);
            }
            return result;
        }

        [[nodiscard]] std::vector<ContactEditorField> mappedFields(const glz::generic& root,
                                                                   const std::string_view mapName,
                                                                   const std::string_view property)
        {
            std::vector<ContactEditorField> result;
            if (!root.is_object() || !root.contains(mapName) || !root.at(mapName).is_object())
                return result;
            for (const auto& [key, entry] : root.at(mapName).get_object())
            {
                if (!entry.is_object())
                    continue;
                ContactEditorField field{.key = key,
                                         .value = stringProperty(entry, property),
                                         .label = std::nullopt,
                                         .preference = std::nullopt,
                                         .contexts = {}};
                const auto label = stringProperty(entry, "label");
                if (!label.empty())
                    field.label = label;
                if (entry.contains("pref") && entry.at("pref").is_number())
                {
                    const auto preference = entry.at("pref").get_number();
                    if (preference >= 1 && preference <= UINT32_MAX)
                        field.preference = static_cast<std::uint32_t>(preference);
                }
                if (entry.contains("contexts") && entry.at("contexts").is_object())
                {
                    for (const auto& [context, enabled] : entry.at("contexts").get_object())
                    {
                        if (enabled.is_boolean())
                            field.contexts.emplace(context, enabled.get_boolean());
                    }
                }
                result.push_back(std::move(field));
            }
            std::ranges::sort(
                result,
                [](const ContactEditorField& left, const ContactEditorField& right)
                {
                    return std::tuple{left.preference.value_or(UINT32_MAX), left.key} <
                           std::tuple{right.preference.value_or(UINT32_MAX), right.key};
                });
            return result;
        }

        void setMappedFields(glz::generic& root, const std::string_view mapName,
                             const std::string_view property,
                             const std::span<const ContactEditorField> fields)
        {
            auto& mapValue = root[mapName];
            if (!mapValue.is_object())
                mapValue.data = glz::generic::object_t{};
            auto& map = mapValue.get_object();
            std::unordered_set<std::string> retainedKeys;
            retainedKeys.reserve(fields.size());
            std::size_t generatedIndex = 1;
            for (const auto& field : fields)
            {
                std::string key = field.key;
                while (key.empty() || retainedKeys.contains(key))
                    key = "javelin-" + std::to_string(generatedIndex++);
                retainedKeys.insert(key);
                auto& entry = mapValue[key];
                if (!entry.is_object())
                    entry.data = glz::generic::object_t{};
                entry[property] = field.value;
                if (field.label.has_value() && !field.label->empty())
                    entry["label"] = *field.label;
                else
                    entry.get_object().erase("label");
                if (field.preference.has_value())
                    entry["pref"] = *field.preference;
                else
                    entry.get_object().erase("pref");
                if (field.contexts.empty())
                    entry.get_object().erase("contexts");
                else
                {
                    auto& contexts = entry["contexts"];
                    contexts.data = glz::generic::object_t{};
                    for (const auto& [context, enabled] : field.contexts)
                        contexts[context] = enabled;
                }
            }
            std::vector<std::string> removedKeys;
            for (const auto& entry : map)
            {
                if (!retainedKeys.contains(entry.first))
                    removedKeys.push_back(entry.first);
            }
            for (const auto& key : removedKeys)
                map.erase(key);
            if (map.empty())
                root.get_object().erase(mapName);
        }

        void setMappedStrings(glz::generic& root, const std::string_view mapName,
                              const std::string_view property,
                              const std::span<const std::string> values)
        {
            auto& mapValue = root[mapName];
            if (!mapValue.is_object())
                mapValue.data = glz::generic::object_t{};
            auto& map = mapValue.get_object();
            std::vector<std::string> keys;
            keys.reserve(map.size());
            for (const auto& entry : map)
                keys.push_back(entry.first);
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                const std::string key =
                    index < keys.size() ? keys[index] : "javelin-" + std::to_string(index + 1);
                auto& entry = mapValue[key];
                if (!entry.is_object())
                    entry.data = glz::generic::object_t{};
                entry[property] = values[index];
            }
            for (std::size_t index = values.size(); index < keys.size(); ++index)
                map.erase(keys[index]);
            if (map.empty())
                root.get_object().erase(mapName);
        }

        void setFirstMappedString(glz::generic& root, const std::string_view mapName,
                                  const std::string_view property, const std::string& value)
        {
            if (value.empty())
            {
                root.get_object().erase(mapName);
                return;
            }
            const std::array values{value};
            setMappedStrings(root, mapName, property, values);
        }
    } // namespace

    std::string normalizeEmail(const std::string_view email)
    {
        std::string normalized{email};
        const auto first = normalized.find_first_not_of(" \t\r\n");
        const auto last = normalized.find_last_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        normalized = normalized.substr(first, last - first + 1);
        std::ranges::transform(normalized, normalized.begin(), [](const unsigned char value)
                               { return static_cast<char>(std::tolower(value)); });
        return normalized;
    }

    std::optional<ContactSummary> summarizeContact(std::string accountId,
                                                   const javelin::jmap::api::ContactCard& card)
    {
        detail::Card parsed;
        auto document = card.document;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(parsed, document))
        {
            return std::nullopt;
        }

        const auto keywordEnabled = [&parsed](const std::string_view keyword)
        {
            const auto found = parsed.keywords.find(std::string{keyword});
            return found != parsed.keywords.end() && found->second;
        };
        ContactSummary summary{.accountId = std::move(accountId),
                               .id = card.id.empty() ? parsed.id : card.id,
                               .uid = card.uid.empty() ? parsed.uid : card.uid,
                               .kind = card.kind.empty() ? parsed.kind : card.kind,
                               .displayName = {},
                               .organization = std::nullopt,
                               .emails = {},
                               .addressBookIds = {},
                               .isImportant =
                                   keywordEnabled("important") || keywordEnabled("starred"),
                               .document = card.document};
        if (parsed.name.has_value())
        {
            if (parsed.name->full.has_value())
            {
                summary.displayName = *parsed.name->full;
            }
            else
            {
                for (const auto& component : parsed.name->components)
                {
                    if (!summary.displayName.empty())
                    {
                        summary.displayName += ' ';
                    }
                    summary.displayName += component.value;
                }
            }
        }
        if (!parsed.organizations.empty())
        {
            summary.organization = parsed.organizations.begin()->second.name;
        }
        for (const auto& [key, email] : parsed.emails)
        {
            summary.emails.push_back(ContactEmail{.key = key,
                                                  .address = email.address,
                                                  .label = email.label,
                                                  .preference = email.pref,
                                                  .contexts = email.contexts});
        }
        std::ranges::sort(summary.emails,
                          [](const ContactEmail& left, const ContactEmail& right)
                          {
                              return left.preference.value_or(UINT32_MAX) <
                                     right.preference.value_or(UINT32_MAX);
                          });
        for (const auto& [id, included] : parsed.addressBookIds)
        {
            if (included)
            {
                summary.addressBookIds.push_back(id);
            }
        }
        if (summary.displayName.empty() && summary.organization.has_value())
        {
            summary.displayName = *summary.organization;
        }
        if (summary.displayName.empty() && !summary.emails.empty())
        {
            summary.displayName = summary.emails.front().address;
        }
        return summary;
    }

    std::variant<std::string, std::string_view> prepareContactDocument(const std::string_view json,
                                                                       const bool creating)
    {
        std::string buffer{json};
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object())
        {
            return std::string_view{"The contact document must be a valid JSON object."};
        }
        auto& object = value.get_object();
        object.erase("id");
        if (creating && (!object.contains("uid") || !object.contains("addressBookIds")))
        {
            return std::string_view{
                "New contacts require uid and at least one addressBookIds entry."};
        }
        if (creating && !object.contains("kind"))
            object.emplace("kind", "individual");
        std::string result;
        if (glz::write_json(value, result))
        {
            return std::string_view{"Unable to serialize the contact document."};
        }
        return result;
    }

    std::variant<std::string, std::string_view> copyContactDocument(std::string contactId,
                                                                    std::string addressBookId)
    {
        if (contactId.empty() || addressBookId.empty())
            return std::string_view{"Copying a contact requires source and destination ids."};

        glz::generic value;
        value["id"] = std::move(contactId);
        auto& addressBookIds = value["addressBookIds"];
        addressBookIds.data = glz::generic::object_t{};
        addressBookIds[std::move(addressBookId)] = true;

        std::string result;
        if (glz::write_json(value, result))
            return std::string_view{"Unable to serialize the copied contact."};
        return result;
    }

    std::variant<std::string, std::string_view>
    setContactPhoto(const std::string_view json, std::string blobId, std::string mediaType)
    {
        std::string buffer{json};
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object())
        {
            return std::string_view{"The contact document must be valid before adding a photo."};
        }
        auto& media = value["media"];
        if (media.is_null())
        {
            media.data = glz::generic::object_t{};
        }
        if (!media.is_object())
        {
            return std::string_view{"The contact media property is not an object."};
        }
        std::string photoKey = "javelin-photo";
        for (const auto& [key, existing] : media.get_object())
        {
            if (stringProperty(existing, "kind") == "photo")
            {
                photoKey = key;
                break;
            }
        }
        glz::generic photo;
        photo["kind"] = std::string{"photo"};
        photo["blobId"] = std::move(blobId);
        photo["mediaType"] = std::move(mediaType);
        media[photoKey] = std::move(photo);
        std::string result;
        if (glz::write_json(value, result))
        {
            return std::string_view{"Unable to update the contact photo."};
        }
        return result;
    }

    std::optional<ContactPhoto> contactPhoto(const std::string_view json)
    {
        std::string buffer{json};
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object() || !value.contains("media") ||
            !value.at("media").is_object())
            return std::nullopt;
        for (const auto& [key, media] : value.at("media").get_object())
        {
            if (stringProperty(media, "kind") != "photo")
                continue;
            const auto optionalProperty =
                [&media](const std::string_view property) -> std::optional<std::string>
            {
                const auto result = stringProperty(media, property);
                return result.empty() ? std::nullopt : std::optional{result};
            };
            return ContactPhoto{.key = key,
                                .blobId = optionalProperty("blobId"),
                                .uri = optionalProperty("uri"),
                                .mediaType = optionalProperty("mediaType")};
        }
        return std::nullopt;
    }

    std::variant<std::string, std::string_view> removeContactPhoto(const std::string_view json)
    {
        std::string buffer{json};
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object())
            return std::string_view{"The contact document must be valid before removing a photo."};
        if (value.contains("media") && !value.at("media").is_object())
            return std::string_view{"The contact media property is not an object."};
        if (value.contains("media"))
        {
            auto& media = value.at("media").get_object();
            std::vector<std::string> photoKeys;
            for (const auto& [key, entry] : media)
            {
                if (stringProperty(entry, "kind") == "photo")
                    photoKeys.push_back(key);
            }
            for (const auto& key : photoKeys)
                media.erase(key);
            if (media.empty())
                value.get_object().erase("media");
        }
        std::string result;
        if (glz::write_json(value, result))
            return std::string_view{"Unable to remove the contact photo."};
        return result;
    }

    std::variant<std::string, std::string_view> setContactStarred(const std::string_view json,
                                                                  const bool starred)
    {
        std::string buffer{json};
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object())
            return std::string_view{"The contact document must be valid before starring it."};

        value.get_object().erase("id");
        if (value.contains("keywords") && !value.at("keywords").is_object())
            return std::string_view{"The contact keywords property is not an object."};
        if (starred)
        {
            auto& keywords = value["keywords"];
            if (keywords.is_null())
                keywords.data = glz::generic::object_t{};
            keywords["starred"] = true;
        }
        else if (value.contains("keywords"))
        {
            auto& keywords = value.at("keywords").get_object();
            keywords.erase("starred");
            keywords.erase("important");
            if (keywords.empty())
                value.get_object().erase("keywords");
        }

        std::string result;
        if (glz::write_json(value, result))
            return std::string_view{"Unable to update the starred contact state."};
        return result;
    }

    std::variant<ContactEditorData, std::string_view> contactEditorData(const std::string_view json)
    {
        std::string buffer{json};
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object())
            return std::string_view{"The contact document is not a valid JSON object."};

        ContactEditorData data;
        data.document = buffer;
        data.uid = stringProperty(value, "uid");
        data.kind = stringProperty(value, "kind");
        if (value.contains("name"))
        {
            data.fullName = stringProperty(value.at("name"), "full");
            if (data.fullName.empty() && value.at("name").is_object() &&
                value.at("name").contains("components") &&
                value.at("name").at("components").is_array())
            {
                for (const auto& component : value.at("name").at("components").get_array())
                {
                    const auto text = stringProperty(component, "value");
                    if (!text.empty())
                    {
                        if (!data.fullName.empty())
                            data.fullName += ' ';
                        data.fullName += text;
                    }
                }
            }
        }
        const auto organizations = mappedStrings(value, "organizations", "name");
        if (!organizations.empty())
            data.organization = organizations.front();
        const auto titles = mappedStrings(value, "titles", "name");
        if (!titles.empty())
            data.title = titles.front();
        data.emails = mappedFields(value, "emails", "address");
        data.phones = mappedFields(value, "phones", "number");
        data.addresses = mappedFields(value, "addresses", "full");
        for (auto& address : data.addresses)
        {
            if (!address.value.empty())
                continue;
            const auto& entry = value.at("addresses").at(address.key);
            if (entry.is_object() && entry.contains("components") &&
                entry.at("components").is_array())
            {
                for (const auto& component : entry.at("components").get_array())
                {
                    const auto text = stringProperty(component, "value");
                    if (!text.empty())
                    {
                        if (!address.value.empty())
                            address.value += ", ";
                        address.value += text;
                    }
                }
            }
        }
        if (value.contains("members") && value.at("members").is_object())
        {
            for (const auto& [uid, included] : value.at("members").get_object())
            {
                if (included.is_boolean() && included.get_boolean())
                    data.members.push_back(uid);
            }
            std::ranges::sort(data.members);
        }
        const auto notes = mappedStrings(value, "notes", "note");
        if (!notes.empty())
            data.notes = notes.front();
        if (value.contains("anniversaries") && value.at("anniversaries").is_object())
        {
            for (const auto& entry : value.at("anniversaries").get_object())
            {
                if (stringProperty(entry.second, "kind") == "birth")
                {
                    const auto& date = entry.second.at("date");
                    if (date.is_string())
                        data.birthday = date.get_string();
                    else if (date.is_object())
                    {
                        const auto number = [&date](const std::string_view key) -> int
                        {
                            return date.contains(key) && date.at(key).is_number()
                                       ? static_cast<int>(date.at(key).get_number())
                                       : 0;
                        };
                        const int year = number("year");
                        const int month = number("month");
                        const int day = number("day");
                        if (year > 0 && month > 0 && day > 0)
                            data.birthday = std::format("{:04}-{:02}-{:02}", year, month, day);
                        else if (month > 0 && day > 0)
                            data.birthday = std::format("--{:02}-{:02}", month, day);
                        else if (year > 0 && month > 0)
                            data.birthday = std::format("{:04}-{:02}", year, month);
                        else if (year > 0)
                            data.birthday = std::format("{:04}", year);
                    }
                    break;
                }
            }
        }
        if (value.contains("addressBookIds") && value.at("addressBookIds").is_object())
        {
            for (const auto& [id, included] : value.at("addressBookIds").get_object())
            {
                if (included.is_boolean() && included.get_boolean())
                    data.addressBookIds.push_back(id);
            }
        }
        return data;
    }

    std::variant<std::string, std::string_view>
    applyContactEditorData(const ContactEditorData& data, const bool creating)
    {
        std::string buffer = data.document;
        glz::generic value;
        if (glz::read_json(value, buffer) || !value.is_object())
            return std::string_view{"The advanced contact document is not valid JSON."};

        value.get_object().erase("id");
        value["kind"] = data.kind.empty() ? std::string{"individual"} : data.kind;
        auto& name = value["name"];
        if (!name.is_object())
            name.data = glz::generic::object_t{};
        name["full"] = data.fullName;
        setFirstMappedString(value, "organizations", "name", data.organization);
        setFirstMappedString(value, "titles", "name", data.title);
        setMappedFields(value, "emails", "address", data.emails);
        setMappedFields(value, "phones", "number", data.phones);
        setMappedFields(value, "addresses", "full", data.addresses);
        if (data.kind == "group")
        {
            auto& members = value["members"];
            members.data = glz::generic::object_t{};
            for (const auto& uid : data.members)
                members[uid] = true;
        }
        else
            value.get_object().erase("members");
        setFirstMappedString(value, "notes", "note", data.notes);

        if (data.addressBookIds.empty())
            return std::string_view{"Select at least one address book."};
        auto& books = value["addressBookIds"];
        books.data = glz::generic::object_t{};
        for (const auto& id : data.addressBookIds)
            books[id] = true;

        bool hasUnprojectedBirthday = false;
        if (value.contains("anniversaries") && value.at("anniversaries").is_object())
        {
            for (const auto& entry : value.at("anniversaries").get_object())
            {
                if (stringProperty(entry.second, "kind") == "birth" &&
                    !entry.second.contains("date"))
                    hasUnprojectedBirthday = true;
                else if (stringProperty(entry.second, "kind") == "birth" &&
                         !entry.second.at("date").is_string())
                    hasUnprojectedBirthday = true;
            }
        }
        if (data.birthday.empty() && !hasUnprojectedBirthday)
            value.get_object().erase("anniversaries");
        else
        {
            auto& anniversaries = value["anniversaries"];
            if (!anniversaries.is_object())
                anniversaries.data = glz::generic::object_t{};
            std::string birthdayKey = "javelin-birthday";
            for (const auto& entry : anniversaries.get_object())
            {
                if (stringProperty(entry.second, "kind") == "birth")
                {
                    birthdayKey = entry.first;
                    break;
                }
            }
            auto& birthday = anniversaries[birthdayKey];
            birthday.data = glz::generic::object_t{};
            birthday["kind"] = std::string{"birth"};
            glz::generic date;
            date.data = glz::generic::object_t{};
            const auto parts = [&data]
            {
                std::vector<int> result;
                std::size_t start = data.birthday.starts_with("--") ? 2 : 0;
                while (start < data.birthday.size())
                {
                    const auto end = data.birthday.find('-', start);
                    const auto token = data.birthday.substr(start, end - start);
                    if (token.empty() ||
                        !std::ranges::all_of(token, [](const unsigned char character)
                                             { return std::isdigit(character) != 0; }))
                        return std::vector<int>{};
                    result.push_back(std::stoi(token));
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                return result;
            }();
            if (data.birthday.starts_with("--") && parts.size() == 2)
            {
                date["month"] = parts[0];
                date["day"] = parts[1];
            }
            else if (parts.size() >= 1 && parts.size() <= 3)
            {
                date["year"] = parts[0];
                if (parts.size() > 1)
                    date["month"] = parts[1];
                if (parts.size() > 2)
                    date["day"] = parts[2];
            }
            else
                return std::string_view{"Birthday must be YYYY, YYYY-MM, YYYY-MM-DD, or --MM-DD."};
            birthday["date"] = std::move(date);
        }
        if (creating && !value.contains("uid"))
            return std::string_view{"New contacts require a uid."};
        std::string result;
        if (glz::write_json(value, result))
            return std::string_view{"Unable to serialize the contact."};
        return result;
    }

    std::variant<std::string, std::string_view>
    createContactGroupDocument(std::string name, std::string uid, std::string addressBookId)
    {
        if (name.empty() || uid.empty() || addressBookId.empty())
            return std::string_view{"A contact group requires a name, uid, and address book."};
        glz::generic document;
        document["uid"] = std::move(uid);
        document["kind"] = std::string{"group"};
        document["name"]["full"] = std::move(name);
        document["addressBookIds"][std::move(addressBookId)] = true;
        document["members"].data = glz::generic::object_t{};
        std::string result;
        if (glz::write_json(document, result))
            return std::string_view{"Unable to serialize the contact group."};
        return result;
    }

    std::variant<std::string, std::string_view>
    contactGroupMembershipPatch(const std::string_view memberUid, const bool included)
    {
        if (memberUid.empty())
            return std::string_view{"A contact group member requires a uid."};
        const std::array members{std::string{memberUid}};
        return contactGroupMembershipPatch(std::span<const std::string>{members}, included);
    }

    std::variant<std::string, std::string_view>
    contactGroupMembershipPatch(const std::span<const std::string> memberUids, const bool included)
    {
        if (memberUids.empty() || std::ranges::any_of(memberUids, &std::string::empty))
            return std::string_view{"A contact group member requires a uid."};
        glz::generic patch;
        for (const auto& memberUid : memberUids)
        {
            const auto path = javelin::jmap::api::patchPath("members", memberUid);
            if (included)
                patch[path] = true;
            else
                patch[path] = nullptr;
        }
        std::string result;
        if (glz::write_json(patch, result))
            return std::string_view{"Unable to serialize the contact group membership change."};
        return result;
    }

    ContactActionRights
    contactActionRights(const bool accountReadOnly,
                        const std::span<const javelin::jmap::api::AddressBook> addressBooks,
                        const std::span<const std::string> contactAddressBookIds)
    {
        if (accountReadOnly)
            return {};
        const bool mayCreate = std::ranges::any_of(addressBooks, [](const auto& book)
                                                   { return book.myRights.mayWrite; });
        if (contactAddressBookIds.empty())
            return {.mayCreate = mayCreate, .mayModify = false, .mayDestroy = false};
        const bool mayWriteEveryMembership =
            std::ranges::all_of(contactAddressBookIds,
                                [&addressBooks](const std::string& id)
                                {
                                    const auto book = std::ranges::find(
                                        addressBooks, id, &javelin::jmap::api::AddressBook::id);
                                    return book != addressBooks.end() && book->myRights.mayWrite;
                                });
        return {.mayCreate = mayCreate,
                .mayModify = mayWriteEveryMembership,
                .mayDestroy = mayWriteEveryMembership};
    }
} // namespace javelin::jmap::contacts
