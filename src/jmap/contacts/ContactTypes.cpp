#include "jmap/contacts/ContactTypes.h"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <cctype>
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
    static constexpr auto value = glz::object(
        "id", &T::id, "uid", &T::uid, "kind", &T::kind, "addressBookIds", &T::addressBookIds,
        "name", &T::name, "emails", &T::emails, "organizations", &T::organizations);
};

namespace javelin::jmap::contacts
{
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

        ContactSummary summary{.accountId = std::move(accountId),
                               .id = card.id,
                               .uid = card.uid,
                               .kind = card.kind,
                               .displayName = {},
                               .organization = std::nullopt,
                               .emails = {},
                               .addressBookIds = {},
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
} // namespace javelin::jmap::contacts
