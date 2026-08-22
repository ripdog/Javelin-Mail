#include "gui/contacts/ContactsBrowser.h"

#include "gui/contacts/ContactsItemRoles.h"

#include <QComboBox>
#include <QListWidget>

#include <algorithm>
#include <ranges>

namespace javelin::gui::contacts
{

    ContactsBrowser::ContactsBrowser(
        QComboBox& accountCombo, QComboBox& addressBookCombo, QListWidget& groupList,
        QListWidget& contactList,
        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
        const std::vector<javelin::jmap::contacts::ContactSummary>& groups)
        : m_accountCombo(accountCombo), m_addressBookCombo(addressBookCombo),
          m_groupList(groupList), m_contactList(contactList), m_contacts(contacts), m_groups(groups)
    {
    }

    std::optional<std::string> ContactsBrowser::currentAccountId() const
    {
        if (const auto* item = m_groupList.currentItem())
        {
            const auto mode = static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt());
            const auto accountId = item->data(groupAccountIdRole).toString();
            if (mode != GroupFilterMode::All && mode != GroupFilterMode::Ungrouped &&
                !accountId.isEmpty())
                return accountId.toStdString();
        }
        const auto contacts = selectedContacts();
        if (contacts.size() == 1)
            return contacts.front()->accountId;
        if (const auto* group = currentGroup())
            return group->accountId;
        const auto value = m_accountCombo.currentData().toString();
        return value.isEmpty() ? std::nullopt : std::optional{value.toStdString()};
    }

    std::optional<std::string> ContactsBrowser::currentAddressBookId() const
    {
        if (const auto* item = m_groupList.currentItem())
        {
            const auto addressBookId = item->data(groupAddressBookIdRole).toString();
            if (!addressBookId.isEmpty())
                return addressBookId.toStdString();
        }
        const auto value = m_addressBookCombo.currentData().toString();
        return value.isEmpty() ? std::nullopt : std::optional{value.toStdString()};
    }

    const javelin::jmap::contacts::ContactSummary* ContactsBrowser::currentContact() const
    {
        const auto contacts = selectedContacts();
        return contacts.size() == 1 ? contacts.front() : nullptr;
    }

    std::vector<const javelin::jmap::contacts::ContactSummary*>
    ContactsBrowser::selectedContacts() const
    {
        std::vector<const javelin::jmap::contacts::ContactSummary*> result;
        result.reserve(static_cast<std::size_t>(m_contactList.selectedItems().size()));
        for (const auto* item : m_contactList.selectedItems())
        {
            const auto id = item->data(Qt::UserRole).toString().toStdString();
            const auto accountId = item->data(contactAccountIdRole).toString().toStdString();
            const auto found = std::ranges::find_if(
                m_contacts, [&id, &accountId](const auto& contact)
                { return contact.id == id && contact.accountId == accountId; });
            if (found != m_contacts.end())
                result.push_back(&*found);
        }
        return result;
    }

    const javelin::jmap::contacts::ContactSummary* ContactsBrowser::currentGroup() const
    {
        const auto* item = m_groupList.currentItem();
        if (item == nullptr ||
            static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt()) !=
                GroupFilterMode::Group)
            return nullptr;
        const auto id = item->data(groupIdRole).toString().toStdString();
        const auto accountId = item->data(groupAccountIdRole).toString().toStdString();
        const auto found =
            std::ranges::find_if(m_groups, [&id, &accountId](const auto& group)
                                 { return group.id == id && group.accountId == accountId; });
        return found == m_groups.end() ? nullptr : &*found;
    }
} // namespace javelin::gui::contacts
