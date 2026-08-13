#pragma once

#include "jmap/contacts/ContactTypes.h"

#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QListWidget;

namespace javelin::gui::contacts
{
    class ContactsBrowser final
    {
      public:
        ContactsBrowser(QComboBox& accountCombo, QComboBox& addressBookCombo,
                        QListWidget& groupList, QListWidget& contactList,
                        const std::vector<javelin::jmap::contacts::ContactSummary>& contacts,
                        const std::vector<javelin::jmap::contacts::ContactSummary>& groups);

        [[nodiscard]] std::optional<std::string> currentAccountId() const;
        [[nodiscard]] std::optional<std::string> currentAddressBookId() const;
        [[nodiscard]] const javelin::jmap::contacts::ContactSummary* currentContact() const;
        [[nodiscard]] std::vector<const javelin::jmap::contacts::ContactSummary*>
        selectedContacts() const;
        [[nodiscard]] const javelin::jmap::contacts::ContactSummary* currentGroup() const;

      private:
        QComboBox& m_accountCombo;
        QComboBox& m_addressBookCombo;
        QListWidget& m_groupList;
        QListWidget& m_contactList;
        const std::vector<javelin::jmap::contacts::ContactSummary>& m_contacts;
        const std::vector<javelin::jmap::contacts::ContactSummary>& m_groups;
    };
} // namespace javelin::gui::contacts
