#include "gui/mailboxes/MailboxPropertiesDialog.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QVBoxLayout>

#include <utility>

namespace javelin::gui::mailboxes
{
    namespace
    {
        [[nodiscard]] QString yesNo(const bool value)
        {
            return value ? i18nc("@item boolean value", "Yes") : i18nc("@item boolean value", "No");
        }

        void addValue(QFormLayout& layout, const QString& label, QString value, QWidget* parent)
        {
            auto* field = new QLineEdit(std::move(value), parent);
            field->setReadOnly(true);
            layout.addRow(label, field);
        }
    } // namespace

    MailboxPropertiesDialog::MailboxPropertiesDialog(
        QString accountName, QString parentMailboxName,
        const javelin::jmap::cache::MailboxTreeItem& mailbox, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(i18n("%1 Properties", QString::fromStdString(mailbox.name)));
        resize(560, 680);

        auto* layout = new QVBoxLayout(this);

        auto* identityGroup = new QGroupBox(i18n("Mailbox"), this);
        auto* identityLayout = new QFormLayout(identityGroup);
        addValue(*identityLayout, i18n("Name:"), QString::fromStdString(mailbox.name),
                 identityGroup);
        addValue(*identityLayout, i18n("Account:"), std::move(accountName), identityGroup);
        addValue(*identityLayout, i18n("Parent:"),
                 mailbox.parentId.has_value() ? std::move(parentMailboxName)
                                              : i18nc("@item no mailbox parent", "None"),
                 identityGroup);
        addValue(*identityLayout, i18n("Role:"),
                 mailbox.role.has_value() ? QString::fromStdString(*mailbox.role)
                                          : i18nc("@item no mailbox role", "None"),
                 identityGroup);
        addValue(*identityLayout, i18n("Sort order:"), QString::number(mailbox.sortOrder),
                 identityGroup);
        addValue(*identityLayout, i18n("Subscribed:"), yesNo(mailbox.isSubscribed), identityGroup);
        addValue(*identityLayout, i18n("Has child mailboxes:"), yesNo(mailbox.hasChildren),
                 identityGroup);
        layout->addWidget(identityGroup);

        auto* countsGroup = new QGroupBox(i18n("Counts"), this);
        auto* countsLayout = new QFormLayout(countsGroup);
        addValue(*countsLayout, i18n("Total emails:"), QString::number(mailbox.totalEmails),
                 countsGroup);
        addValue(*countsLayout, i18n("Unread emails:"), QString::number(mailbox.unreadEmails),
                 countsGroup);
        addValue(*countsLayout, i18n("Total threads:"), QString::number(mailbox.totalThreads),
                 countsGroup);
        addValue(*countsLayout, i18n("Unread threads:"), QString::number(mailbox.unreadThreads),
                 countsGroup);
        layout->addWidget(countsGroup);

        const auto& rights = mailbox.myRights;
        auto* rightsGroup = new QGroupBox(i18n("My rights"), this);
        auto* rightsLayout = new QFormLayout(rightsGroup);
        addValue(*rightsLayout, i18n("Read items:"), yesNo(rights.mayReadItems), rightsGroup);
        addValue(*rightsLayout, i18n("Add items:"), yesNo(rights.mayAddItems), rightsGroup);
        addValue(*rightsLayout, i18n("Remove items:"), yesNo(rights.mayRemoveItems), rightsGroup);
        addValue(*rightsLayout, i18n("Set seen:"), yesNo(rights.maySetSeen), rightsGroup);
        addValue(*rightsLayout, i18n("Set keywords:"), yesNo(rights.maySetKeywords), rightsGroup);
        addValue(*rightsLayout, i18n("Create child:"), yesNo(rights.mayCreateChild), rightsGroup);
        addValue(*rightsLayout, i18n("Rename:"), yesNo(rights.mayRename), rightsGroup);
        addValue(*rightsLayout, i18n("Delete:"), yesNo(rights.mayDelete), rightsGroup);
        addValue(*rightsLayout, i18n("Submit:"), yesNo(rights.maySubmit), rightsGroup);
        layout->addWidget(rightsGroup);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

} // namespace javelin::gui::mailboxes
