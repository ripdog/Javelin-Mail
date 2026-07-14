#include "gui/mailboxes/MailboxPropertiesDialog.h"

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
            return value ? QStringLiteral("Yes") : QStringLiteral("No");
        }

        void addValue(QFormLayout& layout, const QString& label, QString value, QWidget* parent)
        {
            auto* field = new QLineEdit(std::move(value), parent);
            field->setReadOnly(true);
            layout.addRow(label, field);
        }
    } // namespace

    MailboxPropertiesDialog::MailboxPropertiesDialog(
        QString accountId, const javelin::jmap::cache::MailboxTreeItem& mailbox, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(QStringLiteral("%1 Properties").arg(QString::fromStdString(mailbox.name)));
        resize(560, 680);

        auto* layout = new QVBoxLayout(this);

        auto* identityGroup = new QGroupBox(QStringLiteral("Mailbox"), this);
        auto* identityLayout = new QFormLayout(identityGroup);
        addValue(*identityLayout, QStringLiteral("Name:"), QString::fromStdString(mailbox.name),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("JMAP ID:"), QString::fromStdString(mailbox.id),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("Account JMAP ID:"), std::move(accountId),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("Parent JMAP ID:"),
                 mailbox.parentId.has_value() ? QString::fromStdString(*mailbox.parentId)
                                              : QStringLiteral("None"),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("Role:"),
                 mailbox.role.has_value() ? QString::fromStdString(*mailbox.role)
                                          : QStringLiteral("None"),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("Sort order:"), QString::number(mailbox.sortOrder),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("Subscribed:"), yesNo(mailbox.isSubscribed),
                 identityGroup);
        addValue(*identityLayout, QStringLiteral("Has child mailboxes:"),
                 yesNo(mailbox.hasChildren), identityGroup);
        layout->addWidget(identityGroup);

        auto* countsGroup = new QGroupBox(QStringLiteral("Counts"), this);
        auto* countsLayout = new QFormLayout(countsGroup);
        addValue(*countsLayout, QStringLiteral("Total emails:"),
                 QString::number(mailbox.totalEmails), countsGroup);
        addValue(*countsLayout, QStringLiteral("Unread emails:"),
                 QString::number(mailbox.unreadEmails), countsGroup);
        addValue(*countsLayout, QStringLiteral("Total threads:"),
                 QString::number(mailbox.totalThreads), countsGroup);
        addValue(*countsLayout, QStringLiteral("Unread threads:"),
                 QString::number(mailbox.unreadThreads), countsGroup);
        layout->addWidget(countsGroup);

        const auto& rights = mailbox.myRights;
        auto* rightsGroup = new QGroupBox(QStringLiteral("My rights"), this);
        auto* rightsLayout = new QFormLayout(rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Read items:"), yesNo(rights.mayReadItems),
                 rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Add items:"), yesNo(rights.mayAddItems),
                 rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Remove items:"), yesNo(rights.mayRemoveItems),
                 rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Set seen:"), yesNo(rights.maySetSeen), rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Set keywords:"), yesNo(rights.maySetKeywords),
                 rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Create child:"), yesNo(rights.mayCreateChild),
                 rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Rename:"), yesNo(rights.mayRename), rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Delete:"), yesNo(rights.mayDelete), rightsGroup);
        addValue(*rightsLayout, QStringLiteral("Submit:"), yesNo(rights.maySubmit), rightsGroup);
        layout->addWidget(rightsGroup);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

} // namespace javelin::gui::mailboxes
