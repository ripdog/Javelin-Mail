#include "jmap/contacts/ContactIdentityLookup.h"

#include "jmap/cache/ContactRepository.h"
#include "jmap/contacts/ContactTypes.h"

#include <algorithm>
#include <unordered_set>

namespace javelin::jmap::contacts
{
    ContactIdentityLookup::ContactIdentityLookup(
        javelin::jmap::cache::ContactRepository& repository)
        : m_repository(repository)
    {
        connect(&m_repository, &javelin::jmap::cache::ContactRepository::contactsChanged, this,
                &ContactIdentityLookup::contactDataChanged);
    }

    std::variant<std::optional<ContactIdentity>, javelin::jmap::cache::DatabaseError>
    ContactIdentityLookup::resolve(const std::string_view accountId,
                                   const std::string_view email) const
    {
        auto result = m_repository.findByEmail(normalizeEmail(email), accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            return *error;
        }
        if (!std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(result).has_value())
        {
            result = m_repository.findByEmail(normalizeEmail(email));
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                return *error;
            }
        }
        const auto& contact =
            std::get<std::optional<javelin::jmap::contacts::ContactSummary>>(result);
        if (!contact.has_value())
        {
            return std::optional<ContactIdentity>{std::nullopt};
        }
        const auto match =
            std::ranges::find_if(contact->emails, [&](const ContactEmail& item)
                                 { return normalizeEmail(item.address) == normalizeEmail(email); });
        return std::optional<ContactIdentity>{ContactIdentity{
            .contactId = contact->id,
            .displayName = contact->displayName,
            .organization = contact->organization,
            .email = match == contact->emails.end() ? std::string{email} : match->address,
        }};
    }

    std::variant<std::vector<ContactIdentity>, javelin::jmap::cache::DatabaseError>
    ContactIdentityLookup::suggestions(const std::optional<std::string_view> accountId) const
    {
        std::vector<std::string> accountIds;
        if (accountId.has_value())
        {
            accountIds.emplace_back(*accountId);
        }
        else
        {
            const auto accounts = m_repository.listAccounts();
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&accounts))
            {
                return *error;
            }
            for (const auto& account :
                 std::get<std::vector<javelin::jmap::cache::ContactAccount>>(accounts))
            {
                accountIds.push_back(account.accountId);
            }
        }

        std::vector<ContactIdentity> result;
        std::unordered_set<std::string> seen;
        for (const auto& id : accountIds)
        {
            const auto contacts = m_repository.listContacts(id);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&contacts))
            {
                return *error;
            }
            for (const auto& contact :
                 std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(contacts))
            {
                for (const auto& email : contact.emails)
                {
                    if (!seen.insert(normalizeEmail(email.address)).second)
                    {
                        continue;
                    }
                    result.push_back({.contactId = contact.id,
                                      .displayName = contact.displayName,
                                      .organization = contact.organization,
                                      .email = email.address});
                }
            }
        }
        return result;
    }
} // namespace javelin::jmap::contacts
