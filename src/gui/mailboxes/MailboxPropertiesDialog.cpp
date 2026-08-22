#include "gui/mailboxes/MailboxPropertiesDialog.h"

#include <KLocalizedString>

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
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

        [[nodiscard]] QString mailboxType(const std::optional<std::string>& role)
        {
            if (!role.has_value())
                return i18n("Regular mailbox");
            if (*role == "inbox")
                return i18n("Inbox");
            if (*role == "archive")
                return i18n("Archive");
            if (*role == "drafts")
                return i18n("Drafts");
            if (*role == "scheduled")
                return i18n("Scheduled");
            if (*role == "sent")
                return i18n("Sent");
            if (*role == "junk")
                return i18n("Junk");
            if (*role == "trash")
                return i18n("Trash");
            return i18n("Special mailbox (%1)", QString::fromStdString(*role));
        }

        void addValue(QFormLayout& layout, const QString& label, QString value, QWidget* parent)
        {
            auto* field = new QLabel(std::move(value), parent);
            field->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                           Qt::TextSelectableByKeyboard);
            field->setWordWrap(true);
            layout.addRow(label, field);
        }

        [[nodiscard]] QString
        deletionExplanation(const javelin::jmap::cache::MailboxTreeItem& mailbox)
        {
            if (!mailbox.myRights.mayDelete)
                return i18n("The server does not allow you to delete this mailbox.");
            if (mailbox.hasChildren)
                return i18n("Delete or move its child mailboxes before deleting this mailbox.");
            if (mailbox.totalEmails != 0)
            {
                return i18np("Move or delete the %1 message in this mailbox before deleting it.",
                             "Move or delete the %1 messages in this mailbox before deleting it.",
                             mailbox.totalEmails);
            }
            return i18n("This mailbox is empty and can be deleted. No messages will be deleted.");
        }
    } // namespace

    MailboxPropertiesDialog::MailboxPropertiesDialog(
        QString accountName, QString parentMailboxName,
        const javelin::jmap::cache::MailboxTreeItem& mailbox, const bool availableOffline,
        const bool notificationsEnabled, QWidget* parent)
        : QDialog(parent)
    {
        setWindowTitle(i18n("%1 Properties", QString::fromStdString(mailbox.name)));
        resize(600, 720);

        auto* layout = new QVBoxLayout(this);

        auto* mailboxGroup = new QGroupBox(i18n("Mailbox"), this);
        auto* mailboxLayout = new QFormLayout(mailboxGroup);
        const QString displayedParent = mailbox.parentId.has_value()
                                            ? parentMailboxName
                                            : i18nc("@item no mailbox parent", "None");
        addValue(*mailboxLayout, i18n("Name:"), QString::fromStdString(mailbox.name), mailboxGroup);
        addValue(*mailboxLayout, i18n("Account:"), accountName, mailboxGroup);
        addValue(*mailboxLayout, i18n("Parent:"), displayedParent, mailboxGroup);
        addValue(*mailboxLayout, i18n("Type:"), mailboxType(mailbox.role), mailboxGroup);
        addValue(*mailboxLayout, i18n("Visibility:"),
                 mailbox.isSubscribed ? i18n("Shown") : i18n("Hidden"), mailboxGroup);
        addValue(*mailboxLayout, i18n("Available offline:"), yesNo(availableOffline), mailboxGroup);
        addValue(*mailboxLayout, i18n("New-mail notifications:"), yesNo(notificationsEnabled),
                 mailboxGroup);
        addValue(*mailboxLayout, i18n("Sort priority:"), QString::number(mailbox.sortOrder),
                 mailboxGroup);
        layout->addWidget(mailboxGroup);

        auto* contentsGroup = new QGroupBox(i18n("Contents"), this);
        auto* contentsLayout = new QFormLayout(contentsGroup);
        addValue(*contentsLayout, i18n("Messages:"), QString::number(mailbox.totalEmails),
                 contentsGroup);
        addValue(*contentsLayout, i18n("Unread messages:"), QString::number(mailbox.unreadEmails),
                 contentsGroup);
        addValue(*contentsLayout, i18n("Threads:"), QString::number(mailbox.totalThreads),
                 contentsGroup);
        addValue(*contentsLayout, i18n("Unread threads:"), QString::number(mailbox.unreadThreads),
                 contentsGroup);
        addValue(*contentsLayout, i18n("Child mailboxes:"), yesNo(mailbox.hasChildren),
                 contentsGroup);
        layout->addWidget(contentsGroup);

        const auto& rights = mailbox.myRights;
        auto* rightsGroup = new QGroupBox(i18n("Permissions"), this);
        auto* rightsLayout = new QFormLayout(rightsGroup);
        addValue(*rightsLayout, i18n("Read messages:"), yesNo(rights.mayReadItems), rightsGroup);
        addValue(*rightsLayout, i18n("Add messages:"), yesNo(rights.mayAddItems), rightsGroup);
        addValue(*rightsLayout, i18n("Remove messages:"), yesNo(rights.mayRemoveItems),
                 rightsGroup);
        addValue(*rightsLayout, i18n("Mark messages read:"), yesNo(rights.maySetSeen), rightsGroup);
        addValue(*rightsLayout, i18n("Change tags and flags:"), yesNo(rights.maySetKeywords),
                 rightsGroup);
        addValue(*rightsLayout, i18n("Create child mailboxes:"), yesNo(rights.mayCreateChild),
                 rightsGroup);
        addValue(*rightsLayout, i18n("Rename mailbox:"), yesNo(rights.mayRename), rightsGroup);
        addValue(*rightsLayout, i18n("Delete mailbox:"), yesNo(rights.mayDelete), rightsGroup);
        addValue(*rightsLayout, i18n("Send from mailbox:"), yesNo(rights.maySubmit), rightsGroup);
        layout->addWidget(rightsGroup);

        auto* managementGroup = new QGroupBox(i18n("Management"), this);
        auto* managementLayout = new QVBoxLayout(managementGroup);
        auto* deletionLabel = new QLabel(deletionExplanation(mailbox), managementGroup);
        deletionLabel->setWordWrap(true);
        managementLayout->addWidget(deletionLabel);
        layout->addWidget(managementGroup);

        layout->addStretch(1);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* deleteButton =
            buttons->addButton(i18n("Delete Mailbox…"), QDialogButtonBox::DestructiveRole);
        deleteButton->setObjectName(QStringLiteral("deleteMailboxButton"));
        deleteButton->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete")));
        const bool canDelete = rights.mayDelete && !mailbox.hasChildren && mailbox.totalEmails == 0;
        deleteButton->setEnabled(canDelete);
        if (!canDelete)
            deleteButton->setToolTip(deletionExplanation(mailbox));

        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(
            deleteButton, &QPushButton::clicked, this,
            [this, mailboxName = QString::fromStdString(mailbox.name), accountName, displayedParent]
            {
                QMessageBox confirmation{QMessageBox::Warning, i18n("Delete Mailbox"),
                                         i18n("Delete “%1”?\n\nAccount: %2\nParent: %3",
                                              mailboxName, accountName, displayedParent),
                                         QMessageBox::NoButton, this};
                confirmation.setInformativeText(
                    i18n("The empty mailbox will be permanently removed. No messages will be "
                         "deleted."));
                auto* confirmDelete =
                    confirmation.addButton(i18n("Delete Mailbox"), QMessageBox::DestructiveRole);
                confirmation.addButton(QMessageBox::Cancel);
                confirmation.setDefaultButton(QMessageBox::Cancel);
                confirmation.exec();
                if (confirmation.clickedButton() != confirmDelete)
                    return;
                m_deleteRequested = true;
                accept();
            });
        layout->addWidget(buttons);
    }

    bool MailboxPropertiesDialog::deleteRequested() const
    {
        return m_deleteRequested;
    }

} // namespace javelin::gui::mailboxes
