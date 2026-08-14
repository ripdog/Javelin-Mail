#include "gui/contacts/ContactEditor.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>
#include <QWidget>

namespace javelin::gui::contacts
{
    ContactEditor::ContactEditor(QFormLayout& form, QComboBox& kind, QLineEdit& organization,
                                 QLineEdit& title, QListWidget& members,
                                 QToolButton& groupDetailsToggle, QWidget& emails, QWidget& phones,
                                 QWidget& addresses, QWidget& birthday)
        : m_form(form), m_kind(kind), m_organization(organization), m_title(title),
          m_members(members), m_groupDetailsToggle(groupDetailsToggle), m_emails(emails),
          m_phones(phones), m_addresses(addresses), m_birthday(birthday)
    {
    }

    void ContactEditor::updateKindFields()
    {
        const auto setRowVisible = [this](QWidget& field, const bool visible)
        {
            field.setVisible(visible);
            if (auto* label = m_form.labelForField(&field))
                label->setVisible(visible);
        };
        const bool isGroup = m_kind.currentData().toString() == QStringLiteral("group");
        setRowVisible(m_organization, !isGroup);
        setRowVisible(m_title, !isGroup);
        setRowVisible(m_members, isGroup);
        m_groupDetailsToggle.setVisible(isGroup);
        const bool showContactDetails = !isGroup || m_groupDetailsToggle.isChecked();
        setRowVisible(m_emails, showContactDetails);
        setRowVisible(m_phones, showContactDetails);
        setRowVisible(m_addresses, showContactDetails);
        setRowVisible(m_birthday, !isGroup);
    }

    std::vector<std::string> ContactEditor::checkedMemberUids() const
    {
        std::vector<std::string> members;
        members.reserve(static_cast<std::size_t>(m_members.count()));
        for (int row = 0; row < m_members.count(); ++row)
        {
            const auto* item = m_members.item(row);
            if (item->checkState() == Qt::Checked)
                members.push_back(item->data(Qt::UserRole).toString().toStdString());
        }
        return members;
    }

    std::vector<std::string>
    ContactEditor::checkedAddressBookIds(const QListWidget& addressBooks) const
    {
        std::vector<std::string> ids;
        ids.reserve(static_cast<std::size_t>(addressBooks.count()));
        for (int row = 0; row < addressBooks.count(); ++row)
        {
            const auto* item = addressBooks.item(row);
            if (item->checkState() == Qt::Checked)
                ids.push_back(item->data(Qt::UserRole).toString().toStdString());
        }
        return ids;
    }
} // namespace javelin::gui::contacts
