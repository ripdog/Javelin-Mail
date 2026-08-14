#pragma once

#include <string>
#include <vector>

class QComboBox;
class QFormLayout;
class QLineEdit;
class QListWidget;
class QToolButton;
class QWidget;

namespace javelin::gui::contacts
{
    class ContactEditor final
    {
      public:
        ContactEditor(QFormLayout& form, QComboBox& kind, QLineEdit& organization, QLineEdit& title,
                      QListWidget& members, QToolButton& groupDetailsToggle, QWidget& emails,
                      QWidget& phones, QWidget& addresses, QWidget& birthday);

        void updateKindFields();
        [[nodiscard]] std::vector<std::string> checkedMemberUids() const;
        [[nodiscard]] std::vector<std::string>
        checkedAddressBookIds(const QListWidget& addressBooks) const;

      private:
        QFormLayout& m_form;
        QComboBox& m_kind;
        QLineEdit& m_organization;
        QLineEdit& m_title;
        QListWidget& m_members;
        QToolButton& m_groupDetailsToggle;
        QWidget& m_emails;
        QWidget& m_phones;
        QWidget& m_addresses;
        QWidget& m_birthday;
    };
} // namespace javelin::gui::contacts
