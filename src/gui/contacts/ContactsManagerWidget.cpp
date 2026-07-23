#include "gui/contacts/ContactsManagerWidget.h"
#include "gui/widgets/EmailAddressLineEdit.h"

#include "gui/IconUtils.h"
#include "gui/settings/PreferencesDialog.h"
#include "jmap/contacts/ContactInterchange.h"
#include "jmap/contacts/ContactService.h"

#include <QCoroTask>

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iterator>
#include <unordered_set>

namespace javelin::gui::contacts
{
    namespace
    {
        enum class GroupFilterMode
        {
            All = 0,
            Starred = 1,
            Group = 2,
            Divider = 3,
            Ungrouped = 4,
        };

        constexpr int groupFilterModeRole = Qt::UserRole + 10;
        constexpr int groupIdRole = Qt::UserRole + 11;
        constexpr int contactAccountIdRole = Qt::UserRole + 12;
        constexpr int contactUidRole = Qt::UserRole + 13;

        [[nodiscard]] std::string contactSelectionKey(const std::string_view accountId,
                                                      const std::string_view contactId)
        {
            return std::string{accountId} + '\n' + std::string{contactId};
        }

        class ContactGroupList final : public QListWidget
        {
          public:
            using QListWidget::QListWidget;

            std::function<void(std::string, std::vector<std::string>)> membershipDropped;

          protected:
            void dragEnterEvent(QDragEnterEvent* event) override
            {
                if (event->source() != nullptr)
                    event->acceptProposedAction();
            }

            void dragMoveEvent(QDragMoveEvent* event) override
            {
                const auto* target = itemAt(event->position().toPoint());
                if (target != nullptr &&
                    static_cast<GroupFilterMode>(target->data(groupFilterModeRole).toInt()) ==
                        GroupFilterMode::Group)
                    event->acceptProposedAction();
                else
                    event->ignore();
            }

            void dropEvent(QDropEvent* event) override
            {
                const auto* target = itemAt(event->position().toPoint());
                const auto* source = qobject_cast<QListWidget*>(event->source());
                const auto* contact = source == nullptr ? nullptr : source->currentItem();
                if (target == nullptr || contact == nullptr ||
                    static_cast<GroupFilterMode>(target->data(groupFilterModeRole).toInt()) !=
                        GroupFilterMode::Group)
                {
                    event->ignore();
                    return;
                }
                const auto groupId = target->data(groupIdRole).toString().toStdString();
                std::vector<std::string> memberUids;
                for (const auto* selected : source->selectedItems())
                {
                    const auto memberUid = selected->data(contactUidRole).toString().toStdString();
                    if (!memberUid.empty())
                        memberUids.push_back(memberUid);
                }
                if (memberUids.empty())
                {
                    const auto memberUid = contact->data(contactUidRole).toString().toStdString();
                    if (!memberUid.empty())
                        memberUids.push_back(memberUid);
                }
                if (groupId.empty() || memberUids.empty() || !membershipDropped)
                {
                    event->ignore();
                    return;
                }
                membershipDropped(std::move(groupId), std::move(memberUids));
                event->acceptProposedAction();
            }
        };
    } // namespace

    class ContactFieldRow final : public QWidget
    {
      public:
        ContactFieldRow(javelin::jmap::contacts::ContactEditorField field,
                        const QString& placeholder, const bool emailAddress, QWidget* parent)
            : QWidget(parent), m_original(std::move(field))
        {
            auto* layout = new QHBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(6);
            const QString value = QString::fromStdString(m_original.value);
            if (emailAddress)
            {
                m_value = new javelin::gui::widgets::EmailAddressLineEdit(value, false, this);
            }
            else
            {
                m_value = new QLineEdit(value, this);
            }
            m_value->setPlaceholderText(placeholder);
            m_context = new QComboBox(this);
            m_context->addItem(QStringLiteral("Other"), QStringLiteral(""));
            m_context->addItem(QStringLiteral("Home"), QStringLiteral("private"));
            m_context->addItem(QStringLiteral("Work"), QStringLiteral("work"));
            const auto activeContexts = std::ranges::count_if(
                m_original.contexts, [](const auto& context) { return context.second; });
            if (activeContexts == 1)
            {
                const auto active = std::ranges::find_if(
                    m_original.contexts, [](const auto& context) { return context.second; });
                if (active == m_original.contexts.end())
                {
                    addPreservedContext();
                }
                else
                {
                    const int index = m_context->findData(QString::fromStdString(active->first));
                    if (index >= 0)
                        m_context->setCurrentIndex(index);
                    else
                        addPreservedContext();
                }
            }
            else if (activeContexts > 0)
                addPreservedContext();
            m_label = new QLineEdit(m_original.label.has_value()
                                        ? QString::fromStdString(*m_original.label)
                                        : QString{},
                                    this);
            m_label->setPlaceholderText(QStringLiteral("Label"));
            m_preference = new QSpinBox(this);
            m_preference->setRange(0, 100);
            m_preference->setSpecialValueText(QStringLiteral("—"));
            m_preference->setValue(static_cast<int>(m_original.preference.value_or(0)));
            m_preference->setToolTip(
                QStringLiteral("Preference rank; 1 is preferred and — is unspecified"));
            m_remove = new QToolButton(this);
            m_remove->setIcon(QIcon::fromTheme(QStringLiteral("list-remove")));
            m_remove->setToolTip(QStringLiteral("Remove field"));
            layout->addWidget(m_value, 1);
            layout->addWidget(m_context);
            layout->addWidget(m_label);
            layout->addWidget(m_preference);
            layout->addWidget(m_remove);
        }

        [[nodiscard]] javelin::jmap::contacts::ContactEditorField field() const
        {
            auto result = m_original;
            result.value = m_value->text().trimmed().toStdString();
            const auto label = m_label->text().trimmed();
            result.label =
                label.isEmpty() ? std::nullopt : std::optional<std::string>{label.toStdString()};
            result.preference = m_preference->value() == 0
                                    ? std::nullopt
                                    : std::optional<std::uint32_t>{
                                          static_cast<std::uint32_t>(m_preference->value())};
            const QString context = m_context->currentData().toString();
            if (context != QStringLiteral("__preserve__"))
            {
                result.contexts.clear();
                if (!context.isEmpty())
                    result.contexts.emplace(context.toStdString(), true);
            }
            return result;
        }

        [[nodiscard]] QToolButton* removeButton() const
        {
            return m_remove;
        }

      private:
        void addPreservedContext()
        {
            m_context->addItem(QStringLiteral("Custom (preserved)"),
                               QStringLiteral("__preserve__"));
            m_context->setCurrentIndex(m_context->count() - 1);
        }

        javelin::jmap::contacts::ContactEditorField m_original;
        QLineEdit* m_value = nullptr;
        QComboBox* m_context = nullptr;
        QLineEdit* m_label = nullptr;
        QSpinBox* m_preference = nullptr;
        QToolButton* m_remove = nullptr;
    };

    class ContactFieldEditor final : public QWidget
    {
      public:
        ContactFieldEditor(QString placeholder, const bool emailAddresses, QWidget* parent)
            : QWidget(parent), m_placeholder(std::move(placeholder)),
              m_emailAddresses(emailAddresses)
        {
            auto* layout = new QVBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(6);
            auto* headers = new QHBoxLayout();
            headers->setContentsMargins(0, 0, 0, 0);
            headers->addWidget(new QLabel(QStringLiteral("Value"), this), 1);
            headers->addWidget(new QLabel(QStringLiteral("Type"), this));
            headers->addWidget(new QLabel(QStringLiteral("Label"), this));
            headers->addWidget(new QLabel(QStringLiteral("Pref"), this));
            headers->addSpacing(24);
            layout->addLayout(headers);
            m_rows = new QVBoxLayout();
            m_rows->setContentsMargins(0, 0, 0, 0);
            m_rows->setSpacing(6);
            layout->addLayout(m_rows);
            auto* add = new QPushButton(QStringLiteral("Add"), this);
            add->setIcon(QIcon::fromTheme(QStringLiteral("list-add")));
            connect(add, &QPushButton::clicked, this, [this] { addRow({}); });
            auto* addRowLayout = new QHBoxLayout();
            addRowLayout->setContentsMargins(0, 0, 0, 0);
            addRowLayout->addWidget(add);
            addRowLayout->addStretch(1);
            layout->addLayout(addRowLayout);
        }

        void setFields(const std::vector<javelin::jmap::contacts::ContactEditorField>& fields)
        {
            while (auto* item = m_rows->takeAt(0))
            {
                delete item->widget();
                delete item;
            }
            for (const auto& field : fields)
                addRow(field);
        }

        [[nodiscard]] std::vector<javelin::jmap::contacts::ContactEditorField> fields() const
        {
            std::vector<javelin::jmap::contacts::ContactEditorField> result;
            for (int index = 0; index < m_rows->count(); ++index)
            {
                const auto* row = static_cast<ContactFieldRow*>(m_rows->itemAt(index)->widget());
                auto field = row->field();
                if (!field.value.empty())
                    result.push_back(std::move(field));
            }
            return result;
        }

      private:
        void addRow(javelin::jmap::contacts::ContactEditorField field)
        {
            auto* row =
                new ContactFieldRow(std::move(field), m_placeholder, m_emailAddresses, this);
            connect(row->removeButton(), &QToolButton::clicked, this,
                    [this, row]
                    {
                        m_rows->removeWidget(row);
                        row->deleteLater();
                    });
            m_rows->addWidget(row);
            row->findChild<QLineEdit*>()->setFocus();
        }

        QString m_placeholder;
        bool m_emailAddresses = false;
        QVBoxLayout* m_rows = nullptr;
    };

    namespace
    {
        class AddressBookDialog final : public QDialog
        {
          public:
            explicit AddressBookDialog(javelin::jmap::api::AddressBook value, QWidget* parent)
                : QDialog(parent), m_value(std::move(value))
            {
                setWindowTitle(m_value.id.empty() ? QStringLiteral("New Address Book")
                                                  : QStringLiteral("Edit Address Book"));
                auto* layout = new QVBoxLayout(this);
                auto* form = new QFormLayout();
                m_name = new QLineEdit(QString::fromStdString(m_value.name), this);
                m_description = new QLineEdit(m_value.description.has_value()
                                                  ? QString::fromStdString(*m_value.description)
                                                  : QString{},
                                              this);
                m_sortOrder = new QSpinBox(this);
                m_sortOrder->setRange(0, INT_MAX);
                m_sortOrder->setValue(static_cast<int>(m_value.sortOrder));
                m_subscription = new QComboBox(this);
                m_subscription->addItem(QStringLiteral("Subscribed"), true);
                m_subscription->addItem(QStringLiteral("Unsubscribed"), false);
                m_subscription->setCurrentIndex(m_value.isSubscribed ? 0 : 1);
                form->addRow(QStringLiteral("Name"), m_name);
                form->addRow(QStringLiteral("Description"), m_description);
                form->addRow(QStringLiteral("Sort order"), m_sortOrder);
                form->addRow(QStringLiteral("Visibility"), m_subscription);
                layout->addLayout(form);
                auto* buttons =
                    new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
                connect(buttons, &QDialogButtonBox::accepted, this,
                        [this]
                        {
                            if (m_name->text().trimmed().isEmpty() ||
                                m_name->text().toUtf8().size() > 255)
                            {
                                QMessageBox::warning(
                                    this, QStringLiteral("Invalid Address Book"),
                                    QStringLiteral("The name must contain 1 to 255 UTF-8 bytes."));
                                return;
                            }
                            m_value.name = m_name->text().trimmed().toStdString();
                            m_value.description = m_description->text().isEmpty()
                                                      ? std::nullopt
                                                      : std::optional<std::string>{
                                                            m_description->text().toStdString()};
                            m_value.sortOrder = static_cast<std::uint32_t>(m_sortOrder->value());
                            m_value.isSubscribed = m_subscription->currentData().toBool();
                            accept();
                        });
                connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
                layout->addWidget(buttons);
            }

            [[nodiscard]] const javelin::jmap::api::AddressBook& value() const
            {
                return m_value;
            }

          private:
            javelin::jmap::api::AddressBook m_value;
            QLineEdit* m_name = nullptr;
            QLineEdit* m_description = nullptr;
            QSpinBox* m_sortOrder = nullptr;
            QComboBox* m_subscription = nullptr;
        };

        class SharingDialog final : public QDialog
        {
          public:
            explicit SharingDialog(
                std::optional<
                    std::unordered_map<std::string, javelin::jmap::api::AddressBookRights>>
                    sharing,
                QWidget* parent)
                : QDialog(parent)
            {
                setWindowTitle(QStringLiteral("Address Book Sharing"));
                resize(720, 360);
                auto* layout = new QVBoxLayout(this);
                m_table = new QTableWidget(0, 5, this);
                m_table->setHorizontalHeaderLabels(
                    {QStringLiteral("Principal ID"), QStringLiteral("Read"),
                     QStringLiteral("Write"), QStringLiteral("Share"), QStringLiteral("Delete")});
                m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
                for (const auto& [principal, rights] : sharing.value_or(
                         std::unordered_map<std::string, javelin::jmap::api::AddressBookRights>{}))
                {
                    addRow(QString::fromStdString(principal), rights);
                }
                layout->addWidget(m_table);
                auto* rowButtons = new QHBoxLayout();
                auto* add = new QPushButton(QStringLiteral("Add Principal"), this);
                auto* remove = new QPushButton(QStringLiteral("Remove"), this);
                rowButtons->addWidget(add);
                rowButtons->addWidget(remove);
                rowButtons->addStretch(1);
                layout->addLayout(rowButtons);
                auto* buttons =
                    new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
                layout->addWidget(buttons);
                connect(add, &QPushButton::clicked, this,
                        [this]
                        {
                            bool accepted = false;
                            const QString principal = QInputDialog::getText(
                                this, QStringLiteral("Principal"), QStringLiteral("Principal ID"),
                                QLineEdit::Normal, QString{}, &accepted);
                            if (accepted && !principal.trimmed().isEmpty())
                            {
                                addRow(principal.trimmed(), {});
                            }
                        });
                connect(remove, &QPushButton::clicked, this,
                        [this]
                        {
                            if (m_table->currentRow() >= 0)
                            {
                                m_table->removeRow(m_table->currentRow());
                            }
                        });
                connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
                connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
            }

            [[nodiscard]] std::optional<
                std::unordered_map<std::string, javelin::jmap::api::AddressBookRights>>
            sharing() const
            {
                if (m_table->rowCount() == 0)
                {
                    return std::nullopt;
                }
                std::unordered_map<std::string, javelin::jmap::api::AddressBookRights> result;
                for (int row = 0; row < m_table->rowCount(); ++row)
                {
                    result.emplace(
                        m_table->item(row, 0)->text().toStdString(),
                        javelin::jmap::api::AddressBookRights{.mayRead = checked(row, 1),
                                                              .mayWrite = checked(row, 2),
                                                              .mayShare = checked(row, 3),
                                                              .mayDelete = checked(row, 4)});
                }
                return result;
            }

          private:
            void addRow(const QString& principal,
                        const javelin::jmap::api::AddressBookRights& rights)
            {
                const int row = m_table->rowCount();
                m_table->insertRow(row);
                m_table->setItem(row, 0, new QTableWidgetItem(principal));
                for (int column = 1; column < 5; ++column)
                {
                    auto* item = new QTableWidgetItem();
                    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                    const bool value = column == 1   ? rights.mayRead
                                       : column == 2 ? rights.mayWrite
                                       : column == 3 ? rights.mayShare
                                                     : rights.mayDelete;
                    item->setCheckState(value ? Qt::Checked : Qt::Unchecked);
                    m_table->setItem(row, column, item);
                }
            }

            [[nodiscard]] bool checked(const int row, const int column) const
            {
                return m_table->item(row, column)->checkState() == Qt::Checked;
            }

            QTableWidget* m_table = nullptr;
        };

        [[nodiscard]] QString accountLabel(const javelin::jmap::cache::ContactAccount& account)
        {
            const auto settings = javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
                QString::fromStdString(account.accountId));
            if (!settings.displayName.isEmpty())
                return settings.displayName;
            return account.name.empty() ? QString::fromStdString(account.accountId)
                                        : QString::fromStdString(account.name);
        }

    } // namespace

    ContactsManagerWidget::ContactsManagerWidget(
        javelin::jmap::cache::ContactRepository& repository,
        javelin::app::MailApplicationService& service, std::string ownerAccountId, QWidget* parent)
        : QWidget(parent), m_repository(repository), m_service(service),
          m_ownerAccountId(std::move(ownerAccountId))
    {
        setupUi();
        connect(&m_repository, &javelin::jmap::cache::ContactRepository::contactsChanged, this,
                [this](const QString& accountId)
                {
                    if (currentAccountId() != std::optional<std::string>{accountId.toStdString()})
                        return;
                    reloadAddressBooks();
                    reloadContacts();
                });
        reloadAccounts();
    }

    bool ContactsManagerWidget::operationInFlight() const
    {
        return m_busy;
    }

    bool ContactsManagerWidget::hasSelectedContact() const
    {
        return !selectedContacts().empty();
    }

    bool ContactsManagerWidget::hasSingleSelectedContact() const
    {
        return selectedContacts().size() == 1;
    }

    bool ContactsManagerWidget::canCreateContact() const
    {
        const auto* account = currentAccount();
        if (account == nullptr)
            return false;
        const auto selectedBook = currentAddressBookId();
        if (selectedBook.has_value())
        {
            const auto book = std::ranges::find(m_addressBooks, *selectedBook,
                                                &javelin::jmap::api::AddressBook::id);
            return !account->isReadOnly && book != m_addressBooks.end() && book->myRights.mayWrite;
        }
        return javelin::jmap::contacts::contactActionRights(account->isReadOnly, m_addressBooks)
            .mayCreate;
    }

    bool ContactsManagerWidget::canEditContact() const
    {
        const auto* account = currentAccount();
        const auto* contact = currentContact();
        if (account == nullptr || contact == nullptr || contact->accountId != account->accountId)
            return false;
        return javelin::jmap::contacts::contactActionRights(account->isReadOnly, m_addressBooks,
                                                            contact->addressBookIds)
            .mayModify;
    }

    bool ContactsManagerWidget::canDeleteContact() const
    {
        const auto* account = currentAccount();
        const auto* contact = currentContact();
        if (account == nullptr || contact == nullptr || contact->accountId != account->accountId)
            return false;
        return javelin::jmap::contacts::contactActionRights(account->isReadOnly, m_addressBooks,
                                                            contact->addressBookIds)
            .mayDestroy;
    }

    bool ContactsManagerWidget::canCreateGroup() const
    {
        return canCreateContact();
    }

    bool ContactsManagerWidget::canStarSelectedContacts() const
    {
        const auto* account = currentAccount();
        const auto contacts = selectedContacts();
        return account != nullptr && !contacts.empty() &&
               std::ranges::all_of(contacts,
                                   [this, account](const auto* contact)
                                   {
                                       return contact->accountId == account->accountId &&
                                              javelin::jmap::contacts::contactActionRights(
                                                  account->isReadOnly, m_addressBooks,
                                                  contact->addressBookIds)
                                                  .mayModify;
                                   });
    }

    bool ContactsManagerWidget::canAddSelectedContactToGroup() const
    {
        const auto contacts = selectedContacts();
        if (contacts.empty())
            return false;
        return std::ranges::any_of(
            m_groups,
            [this, &contacts](const auto& group)
            {
                if (!groupIsWritable(group))
                    return false;
                const auto parsed = javelin::jmap::contacts::contactEditorData(group.document);
                const auto* groupData =
                    std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
                return groupData != nullptr &&
                       std::ranges::any_of(contacts,
                                           [groupData](const auto* contact)
                                           {
                                               return std::ranges::find(groupData->members,
                                                                        contact->uid) ==
                                                      groupData->members.end();
                                           });
            });
    }

    bool ContactsManagerWidget::canRemoveSelectedContactFromGroup() const
    {
        const auto contacts = selectedContacts();
        if (contacts.empty())
            return false;
        return std::ranges::any_of(
            m_groups,
            [this, &contacts](const auto& group)
            {
                if (!groupIsWritable(group))
                    return false;
                const auto parsed = javelin::jmap::contacts::contactEditorData(group.document);
                const auto* groupData =
                    std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
                return groupData != nullptr &&
                       std::ranges::any_of(contacts,
                                           [groupData](const auto* contact)
                                           {
                                               return std::ranges::find(groupData->members,
                                                                        contact->uid) !=
                                                      groupData->members.end();
                                           });
            });
    }

    ContactsViewState ContactsManagerWidget::viewState() const
    {
        const auto* groupItem = m_groupList->currentItem();
        auto state = ContactsViewState{
            .accountId = currentAccountId().value_or(std::string{}),
            .addressBookId = currentAddressBookId().value_or(std::string{}),
            .contactId = currentContact() == nullptr ? std::string{} : currentContact()->id,
            .filter = m_filterEdit->text(),
            .sortMode = m_sortCombo->currentData().toInt(),
            .groupFilterMode = groupItem == nullptr ? static_cast<int>(GroupFilterMode::All)
                                                    : groupItem->data(groupFilterModeRole).toInt(),
            .groupId = groupItem == nullptr ? std::string{}
                                            : groupItem->data(groupIdRole).toString().toStdString(),
            .selectedContactKeys = {},
        };
        for (const auto* contact : selectedContacts())
            state.selectedContactKeys.push_back(
                contactSelectionKey(contact->accountId, contact->id));
        return state;
    }

    void ContactsManagerWidget::restoreViewState(const ContactsViewState& state)
    {
        if (!state.accountId.empty())
        {
            const int accountIndex =
                m_accountCombo->findData(QString::fromStdString(state.accountId));
            if (accountIndex >= 0)
                m_accountCombo->setCurrentIndex(accountIndex);
        }
        if (!state.addressBookId.empty())
        {
            const int addressBookIndex =
                m_addressBookCombo->findData(QString::fromStdString(state.addressBookId));
            if (addressBookIndex >= 0)
                m_addressBookCombo->setCurrentIndex(addressBookIndex);
        }
        m_filterEdit->setText(state.filter);
        const int sortIndex = m_sortCombo->findData(state.sortMode);
        if (sortIndex >= 0)
            m_sortCombo->setCurrentIndex(sortIndex);
        for (int row = 0; row < m_groupList->count(); ++row)
        {
            auto* item = m_groupList->item(row);
            if (item->data(groupFilterModeRole).toInt() == state.groupFilterMode &&
                (state.groupFilterMode != static_cast<int>(GroupFilterMode::Group) ||
                 item->data(groupIdRole).toString().toStdString() == state.groupId))
            {
                m_groupList->setCurrentItem(item);
                break;
            }
        }
        if (!state.selectedContactKeys.empty())
        {
            const std::unordered_set keys(state.selectedContactKeys.begin(),
                                          state.selectedContactKeys.end());
            QSignalBlocker blocker{m_contactList};
            m_contactList->clearSelection();
            QListWidgetItem* current = nullptr;
            for (int row = 0; row < m_contactList->count(); ++row)
            {
                auto* item = m_contactList->item(row);
                const auto key =
                    contactSelectionKey(item->data(contactAccountIdRole).toString().toStdString(),
                                        item->data(Qt::UserRole).toString().toStdString());
                if (keys.contains(key))
                {
                    item->setSelected(true);
                    current = item;
                }
            }
            if (current != nullptr)
                m_contactList->setCurrentItem(current, QItemSelectionModel::NoUpdate);
            showSelectedContact();
        }
        else if (!state.contactId.empty())
        {
            const QString contactId = QString::fromStdString(state.contactId);
            for (int row = 0; row < m_contactList->count(); ++row)
            {
                auto* item = m_contactList->item(row);
                if (item->data(Qt::UserRole).toString() == contactId)
                {
                    m_contactList->setCurrentItem(item);
                    break;
                }
            }
        }
    }

    void ContactsManagerWidget::setupUi()
    {
        setObjectName(QStringLiteral("contactsManager"));
        setStyleSheet(QStringLiteral(
            "#contactCard { background: palette(base); border: 1px solid palette(mid); "
            "border-radius: 10px; }"
            "#contactCardTitle { color: palette(text); font-weight: 600; }"));
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(12);
        auto* top = new QHBoxLayout();
        m_accountCombo = new QComboBox(this);
        m_addressBookCombo = new QComboBox(this);
        top->addWidget(new QLabel(QStringLiteral("Account"), this));
        top->addWidget(m_accountCombo);
        top->addWidget(new QLabel(QStringLiteral("Address book"), this));
        top->addWidget(m_addressBookCombo, 1);
        root->addLayout(top);

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        auto* groups = new QWidget(splitter);
        auto* groupsLayout = new QVBoxLayout(groups);
        groupsLayout->addWidget(new QLabel(QStringLiteral("Groups"), groups));
        m_groupList = new ContactGroupList(groups);
        m_groupList->setAcceptDrops(true);
        m_groupList->setDragDropMode(QAbstractItemView::DropOnly);
        m_groupList->setDefaultDropAction(Qt::CopyAction);
        m_groupList->setMinimumWidth(165);
        static_cast<ContactGroupList*>(m_groupList)->membershipDropped =
            [this](std::string groupId, std::vector<std::string> memberUids)
        { setContactGroupMembership(std::move(groupId), std::move(memberUids), true); };
        groupsLayout->addWidget(m_groupList, 1);
        auto* left = new QWidget(splitter);
        auto* leftLayout = new QVBoxLayout(left);
        auto* searchRow = new QHBoxLayout();
        m_filterEdit = new QLineEdit(left);
        m_filterEdit->setPlaceholderText(QStringLiteral("Filter contacts"));
        m_sortCombo = new QComboBox(left);
        m_sortCombo->addItem(QStringLiteral("Name A–Z"), 0);
        m_sortCombo->addItem(QStringLiteral("Name Z–A"), 1);
        m_sortCombo->addItem(QStringLiteral("Organization"), 2);
        m_sortCombo->addItem(QStringLiteral("Email"), 3);
        searchRow->addWidget(m_filterEdit, 1);
        searchRow->addWidget(m_sortCombo);
        leftLayout->addLayout(searchRow);
        m_contactList = new QListWidget(left);
        m_contactList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_contactList->setContextMenuPolicy(Qt::CustomContextMenu);
        m_contactList->setDragEnabled(true);
        m_contactList->setDragDropMode(QAbstractItemView::DragOnly);
        leftLayout->addWidget(m_contactList, 1);

        m_detailStack = new QStackedWidget(splitter);
        auto* empty = new QLabel(QStringLiteral("Select a contact to view it."), m_detailStack);
        empty->setAlignment(Qt::AlignCenter);
        m_detailStack->addWidget(empty);
        auto* view = new QWidget(m_detailStack);
        auto* viewLayout = new QVBoxLayout(view);
        m_viewTitle = new QLabel(view);
        QFont titleFont = m_viewTitle->font();
        titleFont.setPointSize(titleFont.pointSize() + 5);
        titleFont.setBold(true);
        m_viewTitle->setFont(titleFont);
        m_photoLabel = new QLabel(view);
        m_photoLabel->setFixedSize(88, 88);
        m_photoLabel->setAlignment(Qt::AlignCenter);
        m_photoLabel->setScaledContents(false);
        m_photoLabel->setVisible(false);
        m_starButton = new QToolButton(view);
        m_starButton->setAutoRaise(true);
        m_starButton->setIconSize(QSize(22, 22));
        connect(m_starButton, &QToolButton::clicked, this,
                &ContactsManagerWidget::toggleContactStarred);
        auto* titleLayout = new QHBoxLayout();
        titleLayout->addWidget(m_photoLabel);
        titleLayout->addWidget(m_viewTitle, 1);
        titleLayout->addWidget(m_starButton);
        auto* cardScroll = new QScrollArea(view);
        cardScroll->setWidgetResizable(true);
        cardScroll->setFrameShape(QFrame::NoFrame);
        m_cardContainer = new QWidget(cardScroll);
        m_cardLayout = new QVBoxLayout(m_cardContainer);
        m_cardLayout->setContentsMargins(8, 8, 8, 8);
        m_cardLayout->setSpacing(10);
        cardScroll->setWidget(m_cardContainer);
        auto* fullDocument = new QPlainTextEdit(view);
        fullDocument->setReadOnly(true);
        fullDocument->setObjectName(QStringLiteral("contactDocumentView"));
        fullDocument->setVisible(false);
        auto* viewAdvanced = new QToolButton(view);
        viewAdvanced->setText(QStringLiteral("Advanced details"));
        viewAdvanced->setCheckable(true);
        viewAdvanced->setArrowType(Qt::RightArrow);
        viewAdvanced->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(viewAdvanced, &QToolButton::toggled, fullDocument,
                [viewAdvanced, fullDocument](const bool expanded)
                {
                    viewAdvanced->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                    fullDocument->setVisible(expanded);
                });
        viewLayout->addLayout(titleLayout);
        viewLayout->addWidget(cardScroll, 1);
        viewLayout->addWidget(viewAdvanced);
        viewLayout->addWidget(fullDocument, 1);
        m_detailStack->addWidget(view);
        auto* multiple = new QWidget(m_detailStack);
        auto* multipleLayout = new QVBoxLayout(multiple);
        m_multipleSelectionTitle = new QLabel(multiple);
        QFont multipleTitleFont = m_multipleSelectionTitle->font();
        multipleTitleFont.setPointSize(multipleTitleFont.pointSize() + 5);
        multipleTitleFont.setBold(true);
        m_multipleSelectionTitle->setFont(multipleTitleFont);
        m_multipleStarButton = new QToolButton(multiple);
        m_multipleStarButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(m_multipleStarButton, &QToolButton::clicked, this,
                &ContactsManagerWidget::toggleContactStarred);
        auto* multipleHeader = new QHBoxLayout();
        multipleHeader->addWidget(m_multipleSelectionTitle, 1);
        multipleHeader->addWidget(m_multipleStarButton);
        multipleLayout->addLayout(multipleHeader);
        auto* multipleScroll = new QScrollArea(multiple);
        multipleScroll->setWidgetResizable(true);
        multipleScroll->setFrameShape(QFrame::NoFrame);
        auto* multipleContainer = new QWidget(multipleScroll);
        m_multipleSelectionLayout = new QVBoxLayout(multipleContainer);
        m_multipleSelectionLayout->setContentsMargins(8, 8, 8, 8);
        m_multipleSelectionLayout->setSpacing(0);
        multipleScroll->setWidget(multipleContainer);
        multipleLayout->addWidget(multipleScroll, 1);
        m_detailStack->addWidget(multiple);
        auto* edit = new QWidget(m_detailStack);
        auto* editLayout = new QVBoxLayout(edit);
        auto* scroll = new QScrollArea(edit);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* formWidget = new QWidget(scroll);
        auto* formLayout = new QVBoxLayout(formWidget);
        formLayout->setSpacing(14);
        auto* heading = new QLabel(QStringLiteral("Contact details"), formWidget);
        QFont headingFont = heading->font();
        headingFont.setPointSize(headingFont.pointSize() + 4);
        headingFont.setBold(true);
        heading->setFont(headingFont);
        formLayout->addWidget(heading);

        auto* contactForm = new QFormLayout();
        contactForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_kindEdit = new QComboBox(formWidget);
        m_kindEdit->addItem(QStringLiteral("Person"), QStringLiteral("individual"));
        m_kindEdit->addItem(QStringLiteral("Organization"), QStringLiteral("org"));
        m_kindEdit->addItem(QStringLiteral("Group"), QStringLiteral("group"));
        m_nameEdit = new QLineEdit(formWidget);
        m_nameEdit->setPlaceholderText(QStringLiteral("Full name"));
        m_organizationEdit = new QLineEdit(formWidget);
        m_organizationEdit->setPlaceholderText(QStringLiteral("Company or organization"));
        m_titleEdit = new QLineEdit(formWidget);
        m_titleEdit->setPlaceholderText(QStringLiteral("Role or job title"));
        contactForm->addRow(QStringLiteral("Type"), m_kindEdit);
        contactForm->addRow(QStringLiteral("Name"), m_nameEdit);
        contactForm->addRow(QStringLiteral("Organization"), m_organizationEdit);
        contactForm->addRow(QStringLiteral("Title"), m_titleEdit);

        m_emailsEdit = new ContactFieldEditor(QStringLiteral("Email address"), true, formWidget);
        m_phonesEdit = new ContactFieldEditor(QStringLiteral("Phone number"), false, formWidget);
        m_addressesEdit =
            new ContactFieldEditor(QStringLiteral("Postal address"), false, formWidget);
        contactForm->addRow(QStringLiteral("Emails"), m_emailsEdit);
        contactForm->addRow(QStringLiteral("Phones"), m_phonesEdit);
        contactForm->addRow(QStringLiteral("Addresses"), m_addressesEdit);

        m_membersEdit = new QListWidget(formWidget);
        m_membersEdit->setMaximumHeight(180);
        contactForm->addRow(QStringLiteral("Group members"), m_membersEdit);
        const auto updateGroupFields = [this, contactForm]
        {
            const bool isGroup = m_kindEdit->currentData().toString() == QStringLiteral("group");
            m_membersEdit->setVisible(isGroup);
            if (auto* label = contactForm->labelForField(m_membersEdit))
                label->setVisible(isGroup);
        };
        connect(m_kindEdit, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [updateGroupFields] { updateGroupFields(); });
        updateGroupFields();

        m_birthdayEdit = new QLineEdit(formWidget);
        m_birthdayEdit->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
        m_notesEdit = new QPlainTextEdit(formWidget);
        m_notesEdit->setPlaceholderText(QStringLiteral("Notes"));
        m_notesEdit->setMaximumHeight(100);
        m_addressBooksEdit = new QListWidget(formWidget);
        m_addressBooksEdit->setMaximumHeight(110);
        contactForm->addRow(QStringLiteral("Birthday"), m_birthdayEdit);
        contactForm->addRow(QStringLiteral("Notes"), m_notesEdit);
        contactForm->addRow(QStringLiteral("Address books"), m_addressBooksEdit);
        formLayout->addLayout(contactForm);

        m_advancedToggle = new QToolButton(formWidget);
        m_advancedToggle->setText(QStringLiteral("Advanced and unusual fields"));
        m_advancedToggle->setCheckable(true);
        m_advancedToggle->setArrowType(Qt::RightArrow);
        m_advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_documentEdit = new QPlainTextEdit(edit);
        m_documentEdit->setVisible(false);
        m_documentEdit->setMinimumHeight(220);
        connect(m_advancedToggle, &QToolButton::toggled, m_documentEdit,
                [this](const bool expanded)
                {
                    m_advancedToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                    m_documentEdit->setVisible(expanded);
                });
        formLayout->addWidget(m_advancedToggle);
        formLayout->addWidget(m_documentEdit);
        formLayout->addStretch(1);
        scroll->setWidget(formWidget);
        auto* editButtons = new QHBoxLayout();
        m_editorPhotoLabel = new QLabel(edit);
        m_editorPhotoLabel->setFixedSize(64, 64);
        m_editorPhotoLabel->setAlignment(Qt::AlignCenter);
        m_editorPhotoLabel->setVisible(false);
        editButtons->addWidget(m_editorPhotoLabel);
        m_uploadPhotoButton = new QPushButton(QStringLiteral("Replace Photo…"), edit);
        m_removePhotoButton = new QPushButton(QStringLiteral("Remove Photo"), edit);
        editButtons->addWidget(m_uploadPhotoButton);
        editButtons->addWidget(m_removePhotoButton);
        editButtons->addStretch(1);
        m_cancelButton = new QPushButton(QStringLiteral("Cancel"), edit);
        m_saveButton = new QPushButton(QStringLiteral("Save"), edit);
        editButtons->addWidget(m_cancelButton);
        editButtons->addWidget(m_saveButton);
        editLayout->addWidget(scroll, 1);
        editLayout->addLayout(editButtons);
        m_detailStack->addWidget(edit);
        splitter->addWidget(groups);
        splitter->addWidget(left);
        splitter->addWidget(m_detailStack);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setStretchFactor(2, 2);
        root->addWidget(splitter, 1);

        connect(m_accountCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { reloadAddressBooks(); });
        connect(m_addressBookCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { reloadContacts(); });
        connect(m_filterEdit, &QLineEdit::textChanged, this, [this] { reloadContacts(); });
        connect(m_sortCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { reloadContacts(); });
        connect(m_groupList, &QListWidget::currentRowChanged, this, [this] { reloadContacts(); });
        connect(m_contactList, &QListWidget::itemSelectionChanged, this,
                &ContactsManagerWidget::showSelectedContact);
        connect(m_contactList, &QListWidget::customContextMenuRequested, this,
                &ContactsManagerWidget::showContactContextMenu);
        connect(m_saveButton, &QPushButton::clicked, this, &ContactsManagerWidget::saveContact);
        connect(m_uploadPhotoButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::uploadPhoto);
        connect(m_removePhotoButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::removePhoto);
        connect(m_cancelButton, &QPushButton::clicked, this, &ContactsManagerWidget::cancelEdit);
    }

    void ContactsManagerWidget::reloadAccounts()
    {
        const auto selected = currentAccountId();
        const auto result = m_repository.listAccounts(m_ownerAccountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
        m_accounts = std::get<std::vector<javelin::jmap::cache::ContactAccount>>(result);
        m_accountCombo->clear();
        for (const auto& account : m_accounts)
        {
            m_accountCombo->addItem(accountLabel(account),
                                    QString::fromStdString(account.accountId));
        }
        if (selected.has_value())
        {
            const int index = m_accountCombo->findData(QString::fromStdString(*selected));
            if (index >= 0)
            {
                m_accountCombo->setCurrentIndex(index);
            }
        }
        reloadAddressBooks();
    }

    void ContactsManagerWidget::reloadAddressBooks()
    {
        m_addressBooks.clear();
        m_addressBookCombo->clear();
        m_addressBookCombo->addItem(QStringLiteral("All address books"), QStringLiteral(""));
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
        {
            reloadContacts();
            return;
        }
        const auto result = m_repository.listAddressBooks(*accountId);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
        m_addressBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(result);
        for (const auto& book : m_addressBooks)
        {
            QString label = QString::fromStdString(book.name);
            if (book.isDefault)
            {
                label += QStringLiteral(" (default)");
            }
            if (!book.isSubscribed)
            {
                label += QStringLiteral(" (unsubscribed)");
            }
            m_addressBookCombo->addItem(label, QString::fromStdString(book.id));
        }
        reloadContacts();
    }

    void ContactsManagerWidget::reloadContacts()
    {
        const auto accountId = currentAccountId();
        std::unordered_set<std::string> selectedContactKeys;
        for (const auto* contact : selectedContacts())
            selectedContactKeys.insert(contactSelectionKey(contact->accountId, contact->id));
        const auto* selectedGroupItem = m_groupList->currentItem();
        const auto selectedGroupMode =
            selectedGroupItem == nullptr
                ? GroupFilterMode::All
                : static_cast<GroupFilterMode>(
                      selectedGroupItem->data(groupFilterModeRole).toInt());
        const QString selectedGroupId = selectedGroupItem == nullptr
                                            ? QString{}
                                            : selectedGroupItem->data(groupIdRole).toString();
        const QString selectedId =
            m_contactList->currentItem() == nullptr
                ? QString{}
                : m_contactList->currentItem()->data(Qt::UserRole).toString();
        const QString selectedAccountId =
            m_contactList->currentItem() == nullptr
                ? QString{}
                : m_contactList->currentItem()->data(contactAccountIdRole).toString();
        QSignalBlocker contactSelectionBlocker{m_contactList};
        m_contacts.clear();
        m_groups.clear();
        m_contactList->clear();
        if (!accountId.has_value())
        {
            m_groupList->clear();
            showSelectedContact();
            return;
        }
        const auto bookId = currentAddressBookId();
        const auto unfiltered = m_repository.listContacts(*accountId, std::nullopt);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&unfiltered))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
        for (const auto& contact :
             std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(unfiltered))
        {
            if (contact.kind == "group")
                m_groups.push_back(contact);
        }
        std::ranges::sort(m_groups, {}, &javelin::jmap::contacts::ContactSummary::displayName);

        {
            QSignalBlocker blocker{m_groupList};
            m_groupList->clear();
            const auto addFilter = [this](const QString& label, const GroupFilterMode mode,
                                          const QString& groupId = QString{})
            {
                auto* item = new QListWidgetItem(label, m_groupList);
                item->setData(groupFilterModeRole, static_cast<int>(mode));
                item->setData(groupIdRole, groupId);
                return item;
            };
            addFilter(QStringLiteral("All contacts"), GroupFilterMode::All);
            addFilter(QStringLiteral("Starred contacts"), GroupFilterMode::Starred);
            addFilter(QStringLiteral("No group"), GroupFilterMode::Ungrouped);
            auto* divider = addFilter(QStringLiteral("────────"), GroupFilterMode::Divider);
            divider->setFlags(Qt::NoItemFlags);
            divider->setTextAlignment(Qt::AlignCenter);
            for (const auto& group : m_groups)
            {
                auto* item = addFilter(QString::fromStdString(group.displayName),
                                       GroupFilterMode::Group, QString::fromStdString(group.id));
                item->setIcon(QIcon::fromTheme(QStringLiteral("system-users")));
            }
            QListWidgetItem* restoredGroup = nullptr;
            for (int row = 0; row < m_groupList->count(); ++row)
            {
                auto* item = m_groupList->item(row);
                const auto mode =
                    static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt());
                if (mode == selectedGroupMode &&
                    (mode != GroupFilterMode::Group ||
                     item->data(groupIdRole).toString() == selectedGroupId))
                {
                    restoredGroup = item;
                    break;
                }
            }
            m_groupList->setCurrentItem(restoredGroup == nullptr ? m_groupList->item(0)
                                                                 : restoredGroup);
        }

        const auto activeMode = static_cast<GroupFilterMode>(
            m_groupList->currentItem()->data(groupFilterModeRole).toInt());
        if (activeMode == GroupFilterMode::Group)
        {
            const auto* group = currentGroup();
            const auto parsed =
                group == nullptr ? std::variant<javelin::jmap::contacts::ContactEditorData,
                                                std::string_view>{std::string_view{"Missing group"}}
                                 : javelin::jmap::contacts::contactEditorData(group->document);
            const auto* groupData =
                std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
            if (groupData != nullptr)
            {
                const std::unordered_set memberUids(groupData->members.begin(),
                                                    groupData->members.end());
                std::unordered_set<std::string> listedUids;
                std::vector<std::string> orderedAccounts{*accountId};
                for (const auto& account : m_accounts)
                {
                    if (account.accountId != *accountId)
                        orderedAccounts.push_back(account.accountId);
                }
                for (const auto& memberAccountId : orderedAccounts)
                {
                    const auto listed = m_repository.listContacts(
                        memberAccountId, std::nullopt, m_filterEdit->text().toStdString());
                    const auto* values =
                        std::get_if<std::vector<javelin::jmap::contacts::ContactSummary>>(&listed);
                    if (values == nullptr)
                        continue;
                    for (const auto& contact : *values)
                    {
                        if (contact.kind != "group" && memberUids.contains(contact.uid) &&
                            listedUids.insert(contact.uid).second)
                            m_contacts.push_back(contact);
                    }
                }
            }
        }
        else
        {
            std::unordered_set<std::string> groupedUids;
            if (activeMode == GroupFilterMode::Ungrouped)
            {
                for (const auto& group : m_groups)
                {
                    const auto parsed = javelin::jmap::contacts::contactEditorData(group.document);
                    const auto* groupData =
                        std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
                    if (groupData != nullptr)
                        groupedUids.insert(groupData->members.begin(), groupData->members.end());
                }
            }
            const auto filtered = m_repository.listContacts(
                *accountId,
                bookId.has_value() ? std::optional<std::string_view>{*bookId} : std::nullopt,
                m_filterEdit->text().toStdString());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&filtered))
            {
                Q_EMIT statusMessageRequested(error->message, 10000);
                return;
            }
            for (const auto& contact :
                 std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(filtered))
            {
                if (contact.kind != "group" &&
                    (activeMode != GroupFilterMode::Starred || contact.isImportant) &&
                    (activeMode != GroupFilterMode::Ungrouped ||
                     !groupedUids.contains(contact.uid)))
                    m_contacts.push_back(contact);
            }
        }

        const int sort = m_sortCombo->currentData().toInt();
        std::ranges::sort(m_contacts,
                          [sort](const auto& left, const auto& right)
                          {
                              auto key = [sort](const auto& contact)
                              {
                                  if (sort == 2)
                                      return contact.organization.value_or(contact.displayName);
                                  if (sort == 3 && !contact.emails.empty())
                                      return contact.emails.front().address;
                                  return contact.displayName;
                              };
                              return sort == 1 ? key(left) > key(right) : key(left) < key(right);
                          });
        QListWidgetItem* firstContactItem = nullptr;
        QListWidgetItem* selectedItem = nullptr;
        QListWidgetItem* lastRestoredSelection = nullptr;
        for (const auto& contact : m_contacts)
        {
            auto* item =
                new QListWidgetItem(QString::fromStdString(contact.displayName), m_contactList);
            if (contact.isImportant)
                item->setIcon(javelin::gui::themedSvgIcon(
                    QStringLiteral(":/icons/thunderbird-icons/starred.svg"),
                    m_contactList->palette().color(QPalette::Highlight)));
            QString detail;
            if (contact.organization.has_value())
                detail = QString::fromStdString(*contact.organization);
            else if (!contact.emails.empty())
                detail = QString::fromStdString(contact.emails.front().address);
            item->setToolTip(detail);
            item->setData(Qt::UserRole, QString::fromStdString(contact.id));
            item->setData(contactAccountIdRole, QString::fromStdString(contact.accountId));
            item->setData(contactUidRole, QString::fromStdString(contact.uid));
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
            if (firstContactItem == nullptr)
                firstContactItem = item;
            if (item->data(Qt::UserRole).toString() == selectedId &&
                item->data(contactAccountIdRole).toString() == selectedAccountId)
                selectedItem = item;
            if (selectedContactKeys.contains(contactSelectionKey(contact.accountId, contact.id)))
            {
                item->setSelected(true);
                lastRestoredSelection = item;
            }
        }
        if (firstContactItem != nullptr)
        {
            auto* current = lastRestoredSelection != nullptr
                                ? lastRestoredSelection
                                : (selectedItem == nullptr ? firstContactItem : selectedItem);
            m_contactList->setCurrentItem(current, lastRestoredSelection == nullptr
                                                       ? QItemSelectionModel::ClearAndSelect
                                                       : QItemSelectionModel::NoUpdate);
        }
        showSelectedContact();
    }

    void ContactsManagerWidget::showSelectedContact()
    {
        const auto contacts = selectedContacts();
        if (contacts.size() > 1)
        {
            m_starButton->setEnabled(false);
            rebuildMultipleSelectionSummary(contacts);
            m_detailStack->setCurrentIndex(2);
            Q_EMIT toolbarStateChanged(m_busy, true);
            return;
        }
        const auto* contact = currentContact();
        if (contact == nullptr)
        {
            m_starButton->setEnabled(false);
            m_detailStack->setCurrentIndex(0);
            Q_EMIT toolbarStateChanged(m_busy, false);
            return;
        }
        m_viewTitle->setText(QString::fromStdString(contact->displayName));
        m_starButton->setIcon(javelin::gui::themedSvgIcon(
            contact->isImportant ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                                 : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
            m_starButton->palette().color(contact->isImportant ? QPalette::Highlight
                                                               : QPalette::ButtonText)));
        m_starButton->setToolTip(contact->isImportant ? QStringLiteral("Remove from Starred")
                                                      : QStringLiteral("Add to Starred"));
        m_starButton->setAccessibleName(m_starButton->toolTip());
        m_starButton->setEnabled(!m_busy && canEditContact());
        populateContactCards(*contact);
        showContactPhoto(*contact);
        if (auto* document = m_detailStack->widget(1)->findChild<QPlainTextEdit*>(
                QStringLiteral("contactDocumentView")))
            document->setPlainText(QString::fromStdString(contact->document));
        m_detailStack->setCurrentIndex(1);
        Q_EMIT toolbarStateChanged(m_busy, true);
    }

    void ContactsManagerWidget::rebuildMultipleSelectionSummary(
        const std::vector<const javelin::jmap::contacts::ContactSummary*>& contacts)
    {
        while (auto* item = m_multipleSelectionLayout->takeAt(0))
        {
            if (auto* widget = item->widget())
                widget->deleteLater();
            delete item;
        }

        m_multipleSelectionTitle->setText(
            QStringLiteral("%1 contacts selected").arg(static_cast<qulonglong>(contacts.size())));
        const bool allStarred =
            std::ranges::all_of(contacts, [](const auto* contact) { return contact->isImportant; });
        m_multipleStarButton->setText(allStarred ? QStringLiteral("Remove all from Starred")
                                                 : QStringLiteral("Add all to Starred"));
        m_multipleStarButton->setIcon(javelin::gui::themedSvgIcon(
            allStarred ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                       : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
            m_multipleStarButton->palette().color(allStarred ? QPalette::Highlight
                                                             : QPalette::ButtonText)));
        m_multipleStarButton->setEnabled(!m_busy && canStarSelectedContacts());

        for (std::size_t index = 0; index < contacts.size(); ++index)
        {
            const auto* contact = contacts[index];
            auto* tile = new QWidget(m_multipleSelectionTitle->parentWidget());
            auto* tileLayout = new QVBoxLayout(tile);
            tileLayout->setContentsMargins(4, 10, 4, 10);
            tileLayout->setSpacing(3);
            auto* name = new QLabel(QString::fromStdString(contact->displayName), tile);
            QFont nameFont = name->font();
            nameFont.setBold(true);
            name->setFont(nameFont);
            tileLayout->addWidget(name);

            QStringList details;
            if (contact->organization.has_value())
                details.push_back(QString::fromStdString(*contact->organization));
            if (!contact->emails.empty())
            {
                details.push_back(QString::fromStdString(contact->emails.front().address));
                if (contact->emails.size() > 1)
                    details.push_back(
                        QStringLiteral("+%1 more")
                            .arg(static_cast<qulonglong>(contact->emails.size() - 1)));
            }
            auto* detail = new QLabel(details.join(QStringLiteral(" · ")), tile);
            detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
            detail->setWordWrap(true);
            detail->setVisible(!details.isEmpty());
            tileLayout->addWidget(detail);
            m_multipleSelectionLayout->addWidget(tile);

            if (index + 1 < contacts.size())
            {
                auto* separator = new QFrame(m_multipleSelectionTitle->parentWidget());
                separator->setFrameShape(QFrame::HLine);
                separator->setFrameShadow(QFrame::Plain);
                m_multipleSelectionLayout->addWidget(separator);
            }
        }
        m_multipleSelectionLayout->addStretch(1);
    }

    void ContactsManagerWidget::showContactContextMenu(const QPoint& position)
    {
        auto* item = m_contactList->itemAt(position);
        if (item == nullptr || !(item->flags() & Qt::ItemIsSelectable))
            return;
        if (item->isSelected())
            m_contactList->setCurrentItem(item, QItemSelectionModel::NoUpdate);
        else
            m_contactList->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
        const auto contacts = selectedContacts();
        if (contacts.size() > 1)
        {
            QMenu menu{this};
            const bool allStarred = std::ranges::all_of(contacts, [](const auto* selected)
                                                        { return selected->isImportant; });
            auto* starred = menu.addAction(
                javelin::gui::themedSvgIcon(
                    allStarred ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                               : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
                    m_contactList->palette().color(allStarred ? QPalette::Highlight
                                                              : QPalette::Text)),
                allStarred ? QStringLiteral("Remove all from Starred")
                           : QStringLiteral("Add all to Starred"));
            starred->setEnabled(!m_busy && canStarSelectedContacts());
            connect(starred, &QAction::triggered, this,
                    &ContactsManagerWidget::toggleContactStarred);
            auto* addToGroup = menu.addMenu(QIcon::fromTheme(QStringLiteral("list-add")),
                                            QStringLiteral("Add to Group"));
            populateAddToGroupMenu(*addToGroup);
            auto* removeFromGroup = menu.addMenu(QIcon::fromTheme(QStringLiteral("list-remove")),
                                                 QStringLiteral("Remove from Group"));
            populateRemoveFromGroupMenu(*removeFromGroup);
            menu.exec(m_contactList->viewport()->mapToGlobal(position));
            return;
        }
        const auto* contact = currentContact();
        if (contact == nullptr)
            return;

        QMenu menu{this};
        auto* compose = menu.addAction(QIcon::fromTheme(QStringLiteral("mail-message-new")),
                                       QStringLiteral("Write Message"));
        compose->setEnabled(!contact->emails.empty());
        connect(compose, &QAction::triggered, this,
                [this]
                {
                    const auto* selected = currentContact();
                    if (selected == nullptr || selected->emails.empty())
                        return;
                    Q_EMIT composeMailRequested(
                        m_accountCombo->currentData().toString(),
                        QString::fromStdString(selected->displayName),
                        QString::fromStdString(selected->emails.front().address));
                });
        if (!contact->emails.empty())
        {
            const QString email = QString::fromStdString(contact->emails.front().address);
            auto* copyEmail = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                             QStringLiteral("Copy Email Address"));
            connect(copyEmail, &QAction::triggered, this,
                    [email] { QApplication::clipboard()->setText(email); });
        }
        if (!contact->emails.empty())
        {
            auto* searchMenu = menu.addMenu(QIcon::fromTheme(QStringLiteral("edit-find")),
                                            QStringLiteral("Find Mail From"));
            for (const auto& contactEmail : contact->emails)
            {
                const QString email = QString::fromStdString(contactEmail.address);
                auto* search = searchMenu->addAction(email);
                connect(search, &QAction::triggered, this,
                        [this, email]
                        {
                            Q_EMIT searchMailFromRequested(m_accountCombo->currentData().toString(),
                                                           email);
                        });
            }
        }
        auto* starred = menu.addAction(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/starred.svg"),
                                        m_contactList->palette().color(QPalette::Highlight)),
            contact->isImportant ? QStringLiteral("Remove from Starred")
                                 : QStringLiteral("Add to Starred"));
        starred->setEnabled(!m_busy && canEditContact());
        connect(starred, &QAction::triggered, this, &ContactsManagerWidget::toggleContactStarred);
        auto* addToGroup = menu.addMenu(QIcon::fromTheme(QStringLiteral("list-add")),
                                        QStringLiteral("Add to Group"));
        populateAddToGroupMenu(*addToGroup);
        auto* removeFromGroup = menu.addMenu(QIcon::fromTheme(QStringLiteral("list-remove")),
                                             QStringLiteral("Remove from Group"));
        populateRemoveFromGroupMenu(*removeFromGroup);
        menu.addSeparator();
        auto* edit = menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                    QStringLiteral("Edit Contact"));
        edit->setEnabled(!m_busy && canEditContact());
        connect(edit, &QAction::triggered, this, &ContactsManagerWidget::beginEditContact);
        auto* copy = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                    QStringLiteral("Copy Contact…"));
        connect(copy, &QAction::triggered, this, &ContactsManagerWidget::copyContact);
        auto* exportAction = menu.addAction(QIcon::fromTheme(QStringLiteral("document-export")),
                                            QStringLiteral("Export vCard…"));
        connect(exportAction, &QAction::triggered, this, &ContactsManagerWidget::exportVCard);
        auto* merge = menu.addAction(QIcon::fromTheme(QStringLiteral("merge")),
                                     QStringLiteral("Find and Merge Duplicates…"));
        connect(merge, &QAction::triggered, this, &ContactsManagerWidget::findAndMergeDuplicates);
        auto* remove = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                      QStringLiteral("Delete Contact"));
        connect(remove, &QAction::triggered, this, &ContactsManagerWidget::deleteContact);
        copy->setEnabled(!m_busy);
        exportAction->setEnabled(!m_busy);
        merge->setEnabled(!m_busy && canEditContact());
        remove->setEnabled(!m_busy && canDeleteContact());
        menu.exec(m_contactList->viewport()->mapToGlobal(position));
    }

    void ContactsManagerWidget::populateAddToGroupMenu(QMenu& menu)
    {
        menu.clear();
        const auto contacts = selectedContacts();
        std::vector<std::string> memberUids;
        memberUids.reserve(contacts.size());
        for (const auto* contact : contacts)
            memberUids.push_back(contact->uid);
        bool addedGroup = false;
        if (!contacts.empty())
        {
            for (const auto& group : m_groups)
            {
                const auto parsed = javelin::jmap::contacts::contactEditorData(group.document);
                const auto* groupData =
                    std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
                if (groupData == nullptr || !groupIsWritable(group) ||
                    std::ranges::none_of(contacts,
                                         [groupData](const auto* contact)
                                         {
                                             return std::ranges::find(groupData->members,
                                                                      contact->uid) ==
                                                    groupData->members.end();
                                         }))
                    continue;
                auto* action = menu.addAction(QString::fromStdString(group.displayName));
                connect(action, &QAction::triggered, this, [this, groupId = group.id, memberUids]
                        { setContactGroupMembership(groupId, memberUids, true); });
                addedGroup = true;
            }
        }
        if (!addedGroup)
        {
            auto* unavailable = menu.addAction(QStringLiteral("No available groups"));
            unavailable->setEnabled(false);
        }
        menu.addSeparator();
        auto* create = menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                      QStringLiteral("New Group…"));
        create->setEnabled(!m_busy && canCreateGroup());
        connect(create, &QAction::triggered, this, &ContactsManagerWidget::beginCreateGroup);
    }

    void ContactsManagerWidget::populateRemoveFromGroupMenu(QMenu& menu)
    {
        menu.clear();
        const auto contacts = selectedContacts();
        std::vector<std::string> memberUids;
        memberUids.reserve(contacts.size());
        for (const auto* contact : contacts)
            memberUids.push_back(contact->uid);
        bool addedGroup = false;
        if (!contacts.empty())
        {
            for (const auto& group : m_groups)
            {
                const auto parsed = javelin::jmap::contacts::contactEditorData(group.document);
                const auto* groupData =
                    std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
                if (groupData == nullptr || !groupIsWritable(group) ||
                    std::ranges::none_of(contacts,
                                         [groupData](const auto* contact)
                                         {
                                             return std::ranges::find(groupData->members,
                                                                      contact->uid) !=
                                                    groupData->members.end();
                                         }))
                    continue;
                auto* action = menu.addAction(QString::fromStdString(group.displayName));
                connect(action, &QAction::triggered, this, [this, groupId = group.id, memberUids]
                        { setContactGroupMembership(groupId, memberUids, false); });
                addedGroup = true;
            }
        }
        if (!addedGroup)
        {
            auto* unavailable = menu.addAction(QStringLiteral("Not in a writable group"));
            unavailable->setEnabled(false);
        }
    }

    void ContactsManagerWidget::beginCreateGroup()
    {
        if (m_busy || !canCreateGroup())
            return;
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New Contact Group"),
                                                   QStringLiteral("Group name"), QLineEdit::Normal,
                                                   QString{}, &accepted)
                                 .trimmed();
        if (!accepted || name.isEmpty())
            return;
        std::vector<const javelin::jmap::api::AddressBook*> writableBooks;
        for (const auto& book : m_addressBooks)
        {
            if (book.myRights.mayWrite)
                writableBooks.push_back(&book);
        }
        if (writableBooks.empty())
            return;
        const javelin::jmap::api::AddressBook* target = nullptr;
        if (const auto selected = currentAddressBookId(); selected.has_value())
        {
            const auto found =
                std::ranges::find(m_addressBooks, *selected, &javelin::jmap::api::AddressBook::id);
            if (found != m_addressBooks.end() && found->myRights.mayWrite)
                target = &*found;
        }
        if (target == nullptr && writableBooks.size() == 1)
            target = writableBooks.front();
        if (target == nullptr)
        {
            QStringList labels;
            for (const auto* book : writableBooks)
                labels.push_back(QString::fromStdString(book->name));
            const QString selected =
                QInputDialog::getItem(this, QStringLiteral("New Contact Group"),
                                      QStringLiteral("Address book"), labels, 0, false, &accepted);
            const auto index = labels.indexOf(selected);
            if (!accepted || index < 0)
                return;
            target = writableBooks[static_cast<std::size_t>(index)];
        }
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        setBusy(true);
        auto task = m_service.createContactGroup(
            m_ownerAccountId,
            {.accountId = *accountId, .addressBookId = target->id, .name = name.toStdString()});
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               requestRefresh();
                       });
    }

    void ContactsManagerWidget::setContactGroupMembership(std::string groupId,
                                                          std::vector<std::string> memberUids,
                                                          const bool included)
    {
        if (m_busy || groupId.empty() || memberUids.empty())
            return;
        const auto group =
            std::ranges::find(m_groups, groupId, &javelin::jmap::contacts::ContactSummary::id);
        if (group == m_groups.end() || !groupIsWritable(*group))
            return;
        setBusy(true);
        auto task = m_service.setContactGroupMembership(m_ownerAccountId,
                                                        {.accountId = group->accountId,
                                                         .groupId = std::move(groupId),
                                                         .memberUids = std::move(memberUids),
                                                         .included = included});
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               requestRefresh();
                       });
    }

    void ContactsManagerWidget::toggleContactStarred()
    {
        if (m_busy || !canStarSelectedContacts())
            return;
        const auto accountId = currentAccountId();
        const auto contacts = selectedContacts();
        if (!accountId.has_value() || contacts.empty())
            return;
        const bool starred = !std::ranges::all_of(contacts, [](const auto* contact)
                                                  { return contact->isImportant; });
        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = *accountId;
        for (const auto* contact : contacts)
        {
            const auto document =
                javelin::jmap::contacts::setContactStarred(contact->document, starred);
            if (const auto* message = std::get_if<std::string_view>(&document))
            {
                QMessageBox::warning(
                    this, QStringLiteral("Star Contacts"),
                    QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())));
                return;
            }
            request.update.emplace(contact->id, javelin::jmap::api::ContactDocument{
                                                    .json = std::get<std::string>(document)});
        }
        setBusy(true);
        auto task = m_service.setContactCards(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               Q_EMIT statusMessageRequested(error->message, 10000);
                               return;
                           }
                           requestRefresh();
                       });
    }

    void ContactsManagerWidget::loadEditorDocument(const QString& document)
    {
        const auto parsed = javelin::jmap::contacts::contactEditorData(document.toStdString());
        const auto* editorData = std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
        if (editorData == nullptr)
        {
            QMessageBox::warning(this, QStringLiteral("Contact Editor"),
                                 QStringLiteral("This contact document cannot be edited."));
            return;
        }
        int kindIndex = m_kindEdit->findData(QString::fromStdString(editorData->kind));
        if (kindIndex < 0)
            kindIndex = 0;
        m_kindEdit->setCurrentIndex(kindIndex);
        m_nameEdit->setText(QString::fromStdString(editorData->fullName));
        m_organizationEdit->setText(QString::fromStdString(editorData->organization));
        m_titleEdit->setText(QString::fromStdString(editorData->title));
        m_emailsEdit->setFields(editorData->emails);
        m_phonesEdit->setFields(editorData->phones);
        m_addressesEdit->setFields(editorData->addresses);
        m_birthdayEdit->setText(QString::fromStdString(editorData->birthday));
        m_notesEdit->setPlainText(QString::fromStdString(editorData->notes));
        m_documentEdit->setPlainText(document);
        const bool hasPhoto =
            javelin::jmap::contacts::contactPhoto(document.toStdString()).has_value();
        m_removePhotoButton->setEnabled(hasPhoto);
        if (!hasPhoto)
        {
            m_editorPhotoLabel->clear();
            m_editorPhotoLabel->setVisible(false);
        }
        m_addressBooksEdit->clear();
        for (const auto& book : m_addressBooks)
        {
            auto* item = new QListWidgetItem(QString::fromStdString(book.name), m_addressBooksEdit);
            item->setData(Qt::UserRole, QString::fromStdString(book.id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            if (!book.myRights.mayWrite)
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setCheckState(std::ranges::find(editorData->addressBookIds, book.id) !=
                                        editorData->addressBookIds.end()
                                    ? Qt::Checked
                                    : Qt::Unchecked);
        }
        m_membersEdit->clear();
        std::unordered_set<std::string> availableMembers;
        const std::unordered_set selectedMembers(editorData->members.begin(),
                                                 editorData->members.end());
        for (const auto& account : m_accounts)
        {
            const auto contacts = m_repository.listContacts(account.accountId);
            const auto* values =
                std::get_if<std::vector<javelin::jmap::contacts::ContactSummary>>(&contacts);
            if (values == nullptr)
                continue;
            for (const auto& candidate : *values)
            {
                if (candidate.uid.empty() || candidate.uid == editorData->uid ||
                    !availableMembers.insert(candidate.uid).second)
                    continue;
                auto* item = new QListWidgetItem(
                    QStringLiteral("%1 — %2").arg(QString::fromStdString(candidate.displayName),
                                                  accountLabel(account)),
                    m_membersEdit);
                item->setData(Qt::UserRole, QString::fromStdString(candidate.uid));
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(selectedMembers.contains(candidate.uid) ? Qt::Checked
                                                                            : Qt::Unchecked);
            }
        }
        for (const auto& uid : editorData->members)
        {
            if (availableMembers.contains(uid))
                continue;
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 (currently unavailable)").arg(QString::fromStdString(uid)),
                m_membersEdit);
            item->setData(Qt::UserRole, QString::fromStdString(uid));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        m_advancedToggle->setChecked(false);
        m_detailStack->setCurrentIndex(3);
    }

    void ContactsManagerWidget::beginCreateContact()
    {
        if (m_busy || !canCreateContact())
            return;
        auto bookId = currentAddressBookId();
        if (!bookId.has_value())
        {
            auto defaultBook =
                std::ranges::find_if(m_addressBooks, [](const auto& book)
                                     { return book.isDefault && book.myRights.mayWrite; });
            if (defaultBook == m_addressBooks.end())
                defaultBook = std::ranges::find_if(m_addressBooks, [](const auto& book)
                                                   { return book.myRights.mayWrite; });
            if (defaultBook == m_addressBooks.end())
            {
                QMessageBox::information(this, QStringLiteral("No Address Book"),
                                         QStringLiteral("Create or select an address book first."));
                return;
            }
            bookId = defaultBook->id;
        }
        bool accepted = false;
        const QString type = QInputDialog::getItem(
            this, QStringLiteral("Contact Type"), QStringLiteral("Type"),
            {QStringLiteral("Individual"), QStringLiteral("Organization"), QStringLiteral("Group")},
            0, false, &accepted);
        if (!accepted)
            return;
        const QString kind =
            type == QStringLiteral("Group")
                ? QStringLiteral("group")
                : (type == QStringLiteral("Organization") ? QStringLiteral("org")
                                                          : QStringLiteral("individual"));
        const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString document =
            QStringLiteral("{\n  \"uid\": \"%1\",\n  \"kind\": \"%2\",\n  \"addressBookIds\": "
                           "{\"%3\": true},\n  \"name\": {\"full\": \"\"}%4\n}")
                .arg(uid, kind, QString::fromStdString(*bookId),
                     kind == QStringLiteral("group") ? QStringLiteral(",\n  \"members\": {}")
                                                     : QString{});
        m_creating = true;
        loadEditorDocument(document);
    }

    void ContactsManagerWidget::beginEditContact()
    {
        if (m_busy || !canEditContact())
            return;
        const auto* contact = currentContact();
        if (contact == nullptr)
            return;
        m_creating = false;
        loadEditorDocument(QString::fromStdString(contact->document));
    }

    void ContactsManagerWidget::cancelEdit()
    {
        showSelectedContact();
    }

    void ContactsManagerWidget::saveContact()
    {
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        javelin::jmap::contacts::ContactEditorData editor;
        editor.kind = m_kindEdit->currentData().toString().toStdString();
        editor.fullName = m_nameEdit->text().trimmed().toStdString();
        editor.organization = m_organizationEdit->text().trimmed().toStdString();
        editor.title = m_titleEdit->text().trimmed().toStdString();
        editor.emails = m_emailsEdit->fields();
        editor.phones = m_phonesEdit->fields();
        editor.addresses = m_addressesEdit->fields();
        editor.birthday = m_birthdayEdit->text().trimmed().toStdString();
        editor.notes = m_notesEdit->toPlainText().trimmed().toStdString();
        editor.document = m_documentEdit->toPlainText().toStdString();
        for (int row = 0; row < m_addressBooksEdit->count(); ++row)
        {
            const auto* item = m_addressBooksEdit->item(row);
            if (item->checkState() == Qt::Checked)
                editor.addressBookIds.push_back(item->data(Qt::UserRole).toString().toStdString());
        }
        const auto* account = currentAccount();
        if (account == nullptr || !javelin::jmap::contacts::contactActionRights(
                                       account->isReadOnly, m_addressBooks, editor.addressBookIds)
                                       .mayModify)
        {
            QMessageBox::information(
                this, QStringLiteral("Read-only Contact"),
                QStringLiteral(
                    "You do not have write permission for every selected address book."));
            return;
        }
        if (editor.kind == "group")
        {
            for (int row = 0; row < m_membersEdit->count(); ++row)
            {
                const auto* item = m_membersEdit->item(row);
                if (item->checkState() == Qt::Checked)
                    editor.members.push_back(item->data(Qt::UserRole).toString().toStdString());
            }
        }
        const auto prepared = javelin::jmap::contacts::applyContactEditorData(editor, m_creating);
        if (const auto* message = std::get_if<std::string_view>(&prepared))
        {
            QMessageBox::warning(
                this, QStringLiteral("Invalid Contact"),
                QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())));
            return;
        }
        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = *accountId;
        if (m_creating)
        {
            request.create.emplace("new-contact", javelin::jmap::api::ContactDocument{
                                                      .json = std::get<std::string>(prepared)});
        }
        else if (const auto* contact = currentContact())
        {
            request.update.emplace(contact->id, javelin::jmap::api::ContactDocument{
                                                    .json = std::get<std::string>(prepared)});
        }
        setBusy(true);
        auto task = m_service.setContactCards(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               Q_EMIT statusMessageRequested(error->message, 10000);
                               return;
                           }
                           requestRefresh();
                       });
    }

    void ContactsManagerWidget::uploadPhoto()
    {
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
        {
            return;
        }
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Choose Contact Photo"), QString{},
            QStringLiteral("Images (*.png *.jpg *.jpeg *.gif *.webp *.avif);;All Files (*)"));
        if (path.isEmpty())
        {
            return;
        }
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
        {
            QMessageBox::warning(this, QStringLiteral("Photo Upload"), file.errorString());
            return;
        }
        const QByteArray payload = file.readAll();
        const auto mimeType = QMimeDatabase{}.mimeTypeForFile(path, QMimeDatabase::MatchContent);
        if (!mimeType.name().startsWith(QStringLiteral("image/")))
        {
            QMessageBox::warning(this, QStringLiteral("Photo Upload"),
                                 QStringLiteral("The selected file is not a recognized image."));
            return;
        }
        setBusy(true);
        auto task = m_service.uploadContactMedia(m_ownerAccountId, *accountId, payload,
                                                 mimeType.name().toStdString());
        QCoro::connect(
            std::move(task), this,
            [this, payload](javelin::jmap::contacts::ContactUploadResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    return;
                }
                const auto& media = std::get<javelin::jmap::contacts::UploadedContactMedia>(result);
                const auto document = javelin::jmap::contacts::setContactPhoto(
                    m_documentEdit->toPlainText().toStdString(), media.blobId, media.mediaType);
                if (const auto* message = std::get_if<std::string_view>(&document))
                {
                    QMessageBox::warning(this, QStringLiteral("Photo Upload"),
                                         QString::fromUtf8(message->data(), static_cast<qsizetype>(
                                                                                message->size())));
                    return;
                }
                m_documentEdit->setPlainText(
                    QString::fromStdString(std::get<std::string>(document)));
                QPixmap pixmap;
                if (pixmap.loadFromData(payload))
                {
                    m_editorPhotoLabel->setPixmap(pixmap.scaled(
                        m_editorPhotoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    m_editorPhotoLabel->setVisible(true);
                }
                m_removePhotoButton->setEnabled(true);
                Q_EMIT statusMessageRequested(
                    QStringLiteral("Photo uploaded; save the contact to apply it."), 5000);
            });
    }

    void ContactsManagerWidget::removePhoto()
    {
        const auto document = javelin::jmap::contacts::removeContactPhoto(
            m_documentEdit->toPlainText().toStdString());
        if (const auto* message = std::get_if<std::string_view>(&document))
        {
            QMessageBox::warning(
                this, QStringLiteral("Remove Photo"),
                QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())));
            return;
        }
        m_documentEdit->setPlainText(QString::fromStdString(std::get<std::string>(document)));
        m_editorPhotoLabel->clear();
        m_editorPhotoLabel->setVisible(false);
        m_removePhotoButton->setEnabled(false);
    }

    void
    ContactsManagerWidget::showContactPhoto(const javelin::jmap::contacts::ContactSummary& contact)
    {
        m_photoLabel->clear();
        m_photoLabel->setVisible(false);
        m_editorPhotoLabel->clear();
        m_editorPhotoLabel->setVisible(false);
        const auto photo = javelin::jmap::contacts::contactPhoto(contact.document);
        if (!photo.has_value())
            return;
        const auto show = [this](const QByteArray& payloadBytes)
        {
            QPixmap pixmap;
            if (!pixmap.loadFromData(payloadBytes))
                return;
            m_photoLabel->setPixmap(
                pixmap.scaled(m_photoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_photoLabel->setVisible(true);
            m_editorPhotoLabel->setPixmap(pixmap.scaled(
                m_editorPhotoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            m_editorPhotoLabel->setVisible(true);
        };
        if (photo->uri.has_value() && photo->uri->starts_with("data:"))
        {
            const QByteArray uri = QByteArray::fromStdString(*photo->uri);
            const auto separator = uri.indexOf(',');
            if (separator > 0)
            {
                const auto metadata = uri.first(separator);
                const auto payload = uri.sliced(separator + 1);
                show(metadata.contains(";base64") ? QByteArray::fromBase64(payload)
                                                  : QByteArray::fromPercentEncoding(payload));
            }
            return;
        }
        if (!photo->blobId.has_value())
            return;
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        const std::string contactId = contact.id;
        auto task =
            m_service.downloadContactMedia(m_ownerAccountId, *accountId, *photo->blobId,
                                           photo->mediaType.value_or("application/octet-stream"));
        QCoro::connect(
            std::move(task), this,
            [this, contactId, show](javelin::jmap::contacts::ContactDownloadResult result)
            {
                const auto* selected = currentContact();
                if (selected == nullptr || selected->id != contactId)
                    return;
                if (const auto* media =
                        std::get_if<javelin::jmap::contacts::DownloadedContactMedia>(&result))
                    show(media->data);
            });
    }

    void ContactsManagerWidget::deleteContact()
    {
        if (m_busy)
            return;
        const auto accountId = currentAccountId();
        const auto* contact = currentContact();
        if (!accountId.has_value() || contact == nullptr || !canDeleteContact() ||
            QMessageBox::question(
                this, QStringLiteral("Delete Contact"),
                QStringLiteral("Delete %1?").arg(QString::fromStdString(contact->displayName))) !=
                QMessageBox::Yes)
            return;
        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = *accountId;
        request.destroy.push_back(contact->id);
        setBusy(true);
        auto task = m_service.setContactCards(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               requestRefresh();
                       });
    }

    void ContactsManagerWidget::copyContact()
    {
        if (m_busy)
            return;
        const auto sourceAccountId = currentAccountId();
        const auto* contact = currentContact();
        if (!sourceAccountId.has_value() || contact == nullptr || m_accounts.empty())
            return;

        struct Destination
        {
            javelin::jmap::cache::ContactAccount account;
            std::vector<javelin::jmap::api::AddressBook> books;
        };
        std::vector<Destination> destinations;
        QStringList accountLabels;
        for (const auto& account : m_accounts)
        {
            if (account.isReadOnly)
                continue;
            const auto booksResult = m_repository.listAddressBooks(account.accountId);
            const auto* books =
                std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&booksResult);
            if (books == nullptr)
                continue;
            Destination destination{.account = account, .books = {}};
            std::ranges::copy_if(*books, std::back_inserter(destination.books),
                                 [](const auto& book) { return book.myRights.mayWrite; });
            if (destination.books.empty())
                continue;
            accountLabels.push_back(accountLabel(account));
            destinations.push_back(std::move(destination));
        }
        if (destinations.empty())
        {
            QMessageBox::information(
                this, QStringLiteral("Copy Contact"),
                QStringLiteral("There is no writable destination address book."));
            return;
        }
        bool accepted = false;
        const QString selectedAccount = QInputDialog::getItem(this, QStringLiteral("Copy Contact"),
                                                              QStringLiteral("Destination account"),
                                                              accountLabels, 0, false, &accepted);
        const qsizetype accountIndex = accountLabels.indexOf(selectedAccount);
        if (!accepted || accountIndex < 0)
            return;
        const auto& destination = destinations[static_cast<std::size_t>(accountIndex)];
        QStringList bookLabels;
        for (const auto& book : destination.books)
            bookLabels.push_back(QString::fromStdString(book.name));
        const QString selectedBook = QInputDialog::getItem(
            this, QStringLiteral("Copy Contact"), QStringLiteral("Destination address book"),
            bookLabels, 0, false, &accepted);
        const qsizetype bookIndex = bookLabels.indexOf(selectedBook);
        if (!accepted || bookIndex < 0)
            return;
        const auto& book = destination.books[static_cast<std::size_t>(bookIndex)];

        javelin::jmap::api::ContactCardCopyRequest request;
        request.fromAccountId = *sourceAccountId;
        request.accountId = destination.account.accountId;
        request.create.emplace(
            "copy-contact",
            javelin::jmap::api::ContactDocument{
                .json =
                    QStringLiteral("{\"id\":\"%1\",\"addressBookIds\":{\"%2\":true}}")
                        .arg(QString::fromStdString(contact->id), QString::fromStdString(book.id))
                        .toStdString()});
        setBusy(true);
        auto task = m_service.copyContactCards(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               requestRefresh();
                       });
    }

    void ContactsManagerWidget::exportVCard()
    {
        const auto* contact = currentContact();
        if (m_busy || contact == nullptr)
            return;
        const auto parsed = javelin::jmap::contacts::contactEditorData(contact->document);
        const auto* editorData = std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
        if (editorData == nullptr)
        {
            QMessageBox::warning(this, QStringLiteral("Export vCard"),
                                 QStringLiteral("This contact cannot be exported."));
            return;
        }
        QString suggestedName = QString::fromStdString(contact->displayName);
        suggestedName.replace(QLatin1Char('/'), QLatin1Char('-'));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export vCard"),
                                                          suggestedName + QStringLiteral(".vcf"),
                                                          QStringLiteral("vCard files (*.vcf)"));
        if (path.isEmpty())
            return;
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly))
        {
            QMessageBox::warning(this, QStringLiteral("Export vCard"), file.errorString());
            return;
        }
        const auto output =
            QByteArray::fromStdString(javelin::jmap::contacts::exportVCard(*editorData));
        if (file.write(output) != output.size() || !file.commit())
        {
            QMessageBox::warning(this, QStringLiteral("Export vCard"), file.errorString());
            return;
        }
        Q_EMIT statusMessageRequested(QStringLiteral("Contact exported."), 5000);
    }

    void ContactsManagerWidget::importVCard()
    {
        if (m_busy || !canCreateContact())
            return;
        std::vector<const javelin::jmap::api::AddressBook*> writableBooks;
        for (const auto& book : m_addressBooks)
        {
            if (book.myRights.mayWrite)
                writableBooks.push_back(&book);
        }
        if (writableBooks.empty())
            return;
        const javelin::jmap::api::AddressBook* target = nullptr;
        if (const auto selected = currentAddressBookId(); selected.has_value())
        {
            const auto found =
                std::ranges::find(m_addressBooks, *selected, &javelin::jmap::api::AddressBook::id);
            if (found != m_addressBooks.end() && found->myRights.mayWrite)
                target = &*found;
        }
        if (target == nullptr && writableBooks.size() == 1)
            target = writableBooks.front();
        if (target == nullptr)
        {
            QStringList labels;
            for (const auto* book : writableBooks)
                labels.push_back(QString::fromStdString(book->name));
            bool accepted = false;
            const QString selected = QInputDialog::getItem(
                this, QStringLiteral("Import vCard"), QStringLiteral("Destination address book"),
                labels, 0, false, &accepted);
            const auto index = labels.indexOf(selected);
            if (!accepted || index < 0)
                return;
            target = writableBooks[static_cast<std::size_t>(index)];
        }
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import vCard"), QString{},
            QStringLiteral("vCard files (*.vcf *.vcard);;All files (*)"));
        if (path.isEmpty())
            return;
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
        {
            QMessageBox::warning(this, QStringLiteral("Import vCard"), file.errorString());
            return;
        }
        if (file.size() > 10 * 1024 * 1024)
        {
            QMessageBox::warning(this, QStringLiteral("Import vCard"),
                                 QStringLiteral("The vCard file exceeds 10 MiB."));
            return;
        }
        const auto imported = javelin::jmap::contacts::importVCards(file.readAll().toStdString());
        const auto* contacts =
            std::get_if<std::vector<javelin::jmap::contacts::ContactEditorData>>(&imported);
        if (contacts == nullptr)
        {
            const auto message = std::get<std::string_view>(imported);
            QMessageBox::warning(
                this, QStringLiteral("Import vCard"),
                QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size())));
            return;
        }
        std::unordered_set<std::string> knownUids;
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        const auto cached = m_repository.listContacts(*accountId);
        if (const auto* values =
                std::get_if<std::vector<javelin::jmap::contacts::ContactSummary>>(&cached))
        {
            for (const auto& contact : *values)
                knownUids.insert(contact.uid);
        }
        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = *accountId;
        for (std::size_t index = 0; index < contacts->size(); ++index)
        {
            const auto& importedContact = (*contacts)[index];
            if (!importedContact.uid.empty() && !knownUids.insert(importedContact.uid).second)
            {
                QMessageBox::warning(this, QStringLiteral("Import vCard"),
                                     QStringLiteral("A contact with UID %1 already exists.")
                                         .arg(QString::fromStdString(importedContact.uid)));
                return;
            }
            const auto document = javelin::jmap::contacts::importedContactDocument(
                importedContact, target->id,
                QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
            if (const auto* message = std::get_if<std::string_view>(&document))
            {
                QMessageBox::warning(
                    this, QStringLiteral("Import vCard"),
                    QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())));
                return;
            }
            request.create.emplace(
                "import-" + std::to_string(index + 1),
                javelin::jmap::api::ContactDocument{.json = std::get<std::string>(document)});
        }
        setBusy(true);
        auto task = m_service.setContactCards(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               requestRefresh();
                       });
    }

    void ContactsManagerWidget::findAndMergeDuplicates()
    {
        if (m_busy)
            return;
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        const auto cached = m_repository.listContacts(*accountId);
        const auto* contacts =
            std::get_if<std::vector<javelin::jmap::contacts::ContactSummary>>(&cached);
        if (contacts == nullptr)
            return;
        const auto groups = javelin::jmap::contacts::findDuplicateContacts(*contacts);
        if (groups.empty())
        {
            QMessageBox::information(this, QStringLiteral("Duplicate Contacts"),
                                     QStringLiteral("No likely duplicates were found."));
            return;
        }
        std::size_t groupIndex = 0;
        const auto* selectedContact = currentContact();
        const auto selectedGroup =
            selectedContact == nullptr
                ? groups.end()
                : std::ranges::find_if(groups,
                                       [selectedContact](const auto& group)
                                       {
                                           return std::ranges::find(group.contactIds,
                                                                    selectedContact->id) !=
                                                  group.contactIds.end();
                                       });
        if (selectedGroup != groups.end())
            groupIndex = static_cast<std::size_t>(std::distance(groups.begin(), selectedGroup));
        else if (groups.size() > 1)
        {
            QStringList labels;
            for (const auto& group : groups)
            {
                QStringList names;
                for (const auto& id : group.contactIds)
                {
                    const auto found = std::ranges::find(
                        *contacts, id, &javelin::jmap::contacts::ContactSummary::id);
                    if (found != contacts->end())
                        names.push_back(QString::fromStdString(found->displayName));
                }
                labels.push_back(names.join(QStringLiteral(", ")));
            }
            bool accepted = false;
            const QString selected = QInputDialog::getItem(
                this, QStringLiteral("Duplicate Contacts"), QStringLiteral("Duplicate group"),
                labels, 0, false, &accepted);
            const auto index = labels.indexOf(selected);
            if (!accepted || index < 0)
                return;
            groupIndex = static_cast<std::size_t>(index);
        }
        std::vector<const javelin::jmap::contacts::ContactSummary*> candidates;
        QStringList candidateNames;
        for (const auto& id : groups[groupIndex].contactIds)
        {
            const auto found =
                std::ranges::find(*contacts, id, &javelin::jmap::contacts::ContactSummary::id);
            if (found != contacts->end())
            {
                candidates.push_back(&*found);
                candidateNames.push_back(QString::fromStdString(found->displayName));
            }
        }
        bool accepted = false;
        const QString primaryName =
            QInputDialog::getItem(this, QStringLiteral("Merge Duplicate Contacts"),
                                  QStringLiteral("Keep this contact as the primary"),
                                  candidateNames, 0, false, &accepted);
        const auto primaryIndex = candidateNames.indexOf(primaryName);
        if (!accepted || primaryIndex < 0)
            return;
        const auto* account = currentAccount();
        for (const auto* candidate : candidates)
        {
            if (account == nullptr ||
                !javelin::jmap::contacts::contactActionRights(account->isReadOnly, m_addressBooks,
                                                              candidate->addressBookIds)
                     .mayModify)
            {
                QMessageBox::information(
                    this, QStringLiteral("Merge Duplicate Contacts"),
                    QStringLiteral("Every duplicate must be writable before it can be merged."));
                return;
            }
        }
        if (QMessageBox::question(
                this, QStringLiteral("Merge Duplicate Contacts"),
                QStringLiteral("Merge %1 contacts into %2? This keeps all mapped fields and "
                               "removes the redundant contacts.")
                    .arg(candidates.size())
                    .arg(primaryName)) != QMessageBox::Yes)
            return;
        const auto* primary = candidates[static_cast<std::size_t>(primaryIndex)];
        std::string mergedDocument = primary->document;
        javelin::jmap::api::ContactCardSetRequest request;
        request.accountId = *accountId;
        for (const auto* candidate : candidates)
        {
            if (candidate == primary)
                continue;
            const auto merged =
                javelin::jmap::contacts::mergeContactDocuments(mergedDocument, candidate->document);
            if (const auto* message = std::get_if<std::string_view>(&merged))
            {
                QMessageBox::warning(
                    this, QStringLiteral("Merge Duplicate Contacts"),
                    QString::fromUtf8(message->data(), static_cast<qsizetype>(message->size())));
                return;
            }
            mergedDocument = std::get<std::string>(merged);
            request.destroy.push_back(candidate->id);
        }
        request.update.emplace(primary->id,
                               javelin::jmap::api::ContactDocument{.json = mergedDocument});
        setBusy(true);
        auto task = m_service.setContactCards(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               requestRefresh();
                       });
    }

    void ContactsManagerWidget::requestRefresh()
    {
        if (m_busy)
            return;
        setBusy(true);
        Q_EMIT statusMessageRequested(QStringLiteral("Refreshing contacts…"), 5000);
        auto task = m_service.requestContacts(m_ownerAccountId);
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactRefreshResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               Q_EMIT statusMessageRequested(error->message, 10000);
                               return;
                           }
                           reloadAccounts();
                           const auto& summary =
                               std::get<javelin::jmap::contacts::ContactRefreshSummary>(result);
                           Q_EMIT statusMessageRequested(
                               QStringLiteral("Cached %1 contacts in %2 address books.")
                                   .arg(summary.contactCount)
                                   .arg(summary.addressBookCount),
                               5000);
                       });
    }

    void ContactsManagerWidget::createAddressBook(std::string accountId)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !account->mayCreateAddressBook)
            return;
        javelin::jmap::api::AddressBook book;
        book.isSubscribed = true;
        AddressBookDialog dialog{book, this};
        if (dialog.exec() != QDialog::Accepted)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.create.emplace("new-address-book",
                               javelin::jmap::api::addressBookCreateDocument(dialog.value()));
        applyAddressBookSet(std::move(request), QStringLiteral("Creating address book…"));
    }

    void ContactsManagerWidget::editAddressBook(std::string accountId,
                                                javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !book.myRights.mayWrite)
            return;
        AddressBookDialog dialog{book, this};
        if (dialog.exec() != QDialog::Accepted)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.update.emplace(book.id,
                               javelin::jmap::api::addressBookUpdateDocument(dialog.value()));
        applyAddressBookSet(std::move(request), QStringLiteral("Updating address book…"));
    }

    void ContactsManagerWidget::deleteAddressBook(std::string accountId,
                                                  javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !book.myRights.mayDelete)
            return;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Delete Address Book"),
            QStringLiteral(
                "Delete %1 and remove its contacts that belong to no other address book?")
                .arg(QString::fromStdString(book.name)));
        if (answer != QMessageBox::Yes)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.destroy.push_back(book.id);
        request.onDestroyRemoveContents = true;
        applyAddressBookSet(std::move(request), QStringLiteral("Deleting address book…"));
    }

    void ContactsManagerWidget::setDefaultAddressBook(std::string accountId,
                                                      javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !book.myRights.mayWrite)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.onSuccessSetIsDefault = book.id;
        applyAddressBookSet(std::move(request), QStringLiteral("Changing default address book…"));
    }

    void ContactsManagerWidget::toggleAddressBookSubscription(std::string accountId,
                                                              javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly)
            return;
        auto changed = book;
        changed.isSubscribed = !changed.isSubscribed;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.update.emplace(book.id, javelin::jmap::api::addressBookUpdateDocument(changed));
        applyAddressBookSet(std::move(request), QStringLiteral("Updating subscription…"));
    }

    void ContactsManagerWidget::editAddressBookSharing(std::string accountId,
                                                       javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !book.myRights.mayShare)
        {
            QMessageBox::information(
                this, QStringLiteral("Address Book Sharing"),
                QStringLiteral("You do not have permission to change sharing."));
            return;
        }
        SharingDialog dialog{book.shareWith, this};
        if (dialog.exec() != QDialog::Accepted)
            return;
        auto changed = book;
        changed.shareWith = dialog.sharing();
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.update.emplace(book.id, javelin::jmap::api::addressBookUpdateDocument(changed));
        applyAddressBookSet(std::move(request), QStringLiteral("Updating address book sharing…"));
    }

    void
    ContactsManagerWidget::applyAddressBookSet(javelin::jmap::api::AddressBookSetRequest request,
                                               QString progressMessage)
    {
        setBusy(true);
        Q_EMIT statusMessageRequested(progressMessage, 5000);
        auto task = m_service.setAddressBooks(m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                           {
                               Q_EMIT statusMessageRequested(error->message, 10000);
                               return;
                           }
                           requestRefresh();
                       });
    }

    void ContactsManagerWidget::setBusy(const bool busy)
    {
        m_busy = busy;
        const std::array<QWidget*, 4> buttons{m_saveButton, m_uploadPhotoButton,
                                              m_removePhotoButton, m_cancelButton};
        for (auto* button : buttons)
            button->setEnabled(!busy);
        m_removePhotoButton->setEnabled(!busy && javelin::jmap::contacts::contactPhoto(
                                                     m_documentEdit->toPlainText().toStdString())
                                                     .has_value());
        m_starButton->setEnabled(!busy && canEditContact());
        m_multipleStarButton->setEnabled(!busy && canStarSelectedContacts());
        m_accountCombo->setEnabled(!busy);
        m_addressBookCombo->setEnabled(!busy);
        m_groupList->setEnabled(!busy);
        m_contactList->setEnabled(!busy);
        Q_EMIT toolbarStateChanged(m_busy, hasSelectedContact());
    }

    std::optional<std::string> ContactsManagerWidget::currentAccountId() const
    {
        const QString value = m_accountCombo->currentData().toString();
        return value.isEmpty() ? std::nullopt : std::optional{value.toStdString()};
    }

    std::optional<std::string> ContactsManagerWidget::currentAddressBookId() const
    {
        const QString value = m_addressBookCombo->currentData().toString();
        return value.isEmpty() ? std::nullopt : std::optional{value.toStdString()};
    }

    const javelin::jmap::cache::ContactAccount* ContactsManagerWidget::currentAccount() const
    {
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return nullptr;
        const auto found = std::ranges::find(m_accounts, *accountId,
                                             &javelin::jmap::cache::ContactAccount::accountId);
        return found == m_accounts.end() ? nullptr : &*found;
    }

    const javelin::jmap::contacts::ContactSummary* ContactsManagerWidget::currentContact() const
    {
        const auto contacts = selectedContacts();
        return contacts.size() == 1 ? contacts.front() : nullptr;
    }

    std::vector<const javelin::jmap::contacts::ContactSummary*>
    ContactsManagerWidget::selectedContacts() const
    {
        std::vector<const javelin::jmap::contacts::ContactSummary*> result;
        result.reserve(static_cast<std::size_t>(m_contactList->selectedItems().size()));
        for (const auto* item : m_contactList->selectedItems())
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

    const javelin::jmap::contacts::ContactSummary* ContactsManagerWidget::currentGroup() const
    {
        const auto* item = m_groupList->currentItem();
        if (item == nullptr ||
            static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt()) !=
                GroupFilterMode::Group)
            return nullptr;
        const auto id = item->data(groupIdRole).toString().toStdString();
        const auto found =
            std::ranges::find(m_groups, id, &javelin::jmap::contacts::ContactSummary::id);
        return found == m_groups.end() ? nullptr : &*found;
    }

    bool ContactsManagerWidget::groupIsWritable(
        const javelin::jmap::contacts::ContactSummary& group) const
    {
        const auto* account = currentAccount();
        return account != nullptr && account->accountId == group.accountId &&
               javelin::jmap::contacts::contactActionRights(account->isReadOnly, m_addressBooks,
                                                            group.addressBookIds)
                   .mayModify;
    }

    void ContactsManagerWidget::showAddressBookManager()
    {
        if (m_busy || m_accounts.empty())
            return;
        QDialog dialog{this};
        dialog.setWindowTitle(QStringLiteral("Manage Address Books"));
        dialog.resize(600, 420);
        auto* layout = new QVBoxLayout(&dialog);
        auto* account = new QComboBox(&dialog);
        for (const auto& value : m_accounts)
            account->addItem(accountLabel(value), QString::fromStdString(value.accountId));
        if (const auto current = currentAccountId(); current.has_value())
        {
            const int index = account->findData(QString::fromStdString(*current));
            if (index >= 0)
                account->setCurrentIndex(index);
        }
        layout->addWidget(account);
        auto* books = new QListWidget(&dialog);
        layout->addWidget(books, 1);
        auto* actions = new QHBoxLayout();
        auto* create = new QPushButton(QStringLiteral("New"), &dialog);
        auto* edit = new QPushButton(QStringLiteral("Edit"), &dialog);
        auto* makeDefault = new QPushButton(QStringLiteral("Make Default"), &dialog);
        auto* subscription = new QPushButton(QStringLiteral("Toggle Subscription"), &dialog);
        auto* sharing = new QPushButton(QStringLiteral("Sharing…"), &dialog);
        auto* remove = new QPushButton(QStringLiteral("Delete"), &dialog);
        for (auto* button : {create, edit, makeDefault, subscription, sharing, remove})
            actions->addWidget(button);
        layout->addLayout(actions);

        std::vector<javelin::jmap::api::AddressBook> dialogBooks;
        const auto loadBooks = [this, account, books, &dialogBooks]
        {
            dialogBooks.clear();
            books->clear();
            const auto result =
                m_repository.listAddressBooks(account->currentData().toString().toStdString());
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
            {
                Q_EMIT statusMessageRequested(error->message, 10000);
                return;
            }
            dialogBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(result);
            for (const auto& book : dialogBooks)
            {
                QString label = QString::fromStdString(book.name);
                if (book.isDefault)
                    label += QStringLiteral(" (default)");
                if (!book.isSubscribed)
                    label += QStringLiteral(" (unsubscribed)");
                auto* item = new QListWidgetItem(label, books);
                item->setData(Qt::UserRole, QString::fromStdString(book.id));
            }
            if (!dialogBooks.empty())
                books->setCurrentRow(0);
        };
        const auto selectedBook = [books,
                                   &dialogBooks]() -> std::optional<javelin::jmap::api::AddressBook>
        {
            const auto* item = books->currentItem();
            if (item == nullptr)
                return std::nullopt;
            const auto id = item->data(Qt::UserRole).toString().toStdString();
            const auto found =
                std::ranges::find(dialogBooks, id, &javelin::jmap::api::AddressBook::id);
            return found == dialogBooks.end() ? std::nullopt : std::optional{*found};
        };
        const auto updateActions =
            [this, account, create, edit, makeDefault, subscription, sharing, remove, selectedBook]
        {
            const auto book = selectedBook();
            const bool selected = book.has_value();
            const auto id = account->currentData().toString().toStdString();
            const auto current =
                std::ranges::find(m_accounts, id, &javelin::jmap::cache::ContactAccount::accountId);
            const bool writableAccount = current != m_accounts.end() && !current->isReadOnly;
            create->setEnabled(writableAccount && current->mayCreateAddressBook);
            edit->setEnabled(writableAccount && selected && book->myRights.mayWrite);
            makeDefault->setEnabled(writableAccount && selected && !book->isDefault &&
                                    book->myRights.mayWrite);
            subscription->setEnabled(writableAccount && selected);
            sharing->setEnabled(writableAccount && selected && book->myRights.mayShare);
            remove->setEnabled(writableAccount && selected && book->myRights.mayDelete);
        };
        connect(account, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                [loadBooks, updateActions]
                {
                    loadBooks();
                    updateActions();
                });
        connect(books, &QListWidget::currentItemChanged, &dialog,
                [updateActions](QListWidgetItem*, QListWidgetItem*) { updateActions(); });
        const auto accountId = [account]
        { return account->currentData().toString().toStdString(); };
        connect(create, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId]
                {
                    createAddressBook(accountId());
                    dialog.accept();
                });
        connect(edit, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId, selectedBook]
                {
                    if (auto book = selectedBook())
                    {
                        editAddressBook(accountId(), std::move(*book));
                        dialog.accept();
                    }
                });
        connect(makeDefault, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId, selectedBook]
                {
                    if (auto book = selectedBook())
                    {
                        setDefaultAddressBook(accountId(), std::move(*book));
                        dialog.accept();
                    }
                });
        connect(subscription, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId, selectedBook]
                {
                    if (auto book = selectedBook())
                    {
                        toggleAddressBookSubscription(accountId(), std::move(*book));
                        dialog.accept();
                    }
                });
        connect(sharing, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId, selectedBook]
                {
                    if (auto book = selectedBook())
                    {
                        editAddressBookSharing(accountId(), std::move(*book));
                        dialog.accept();
                    }
                });
        connect(remove, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId, selectedBook]
                {
                    if (auto book = selectedBook())
                    {
                        deleteAddressBook(accountId(), std::move(*book));
                        dialog.accept();
                    }
                });
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(close);
        loadBooks();
        updateActions();
        dialog.exec();
    }

    void ContactsManagerWidget::populateContactCards(
        const javelin::jmap::contacts::ContactSummary& contact)
    {
        while (auto* item = m_cardLayout->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
        const auto parsed = javelin::jmap::contacts::contactEditorData(contact.document);
        const auto* editorData = std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
        if (editorData == nullptr)
        {
            m_cardLayout->addWidget(new QLabel(
                QStringLiteral("This contact could not be displayed."), m_cardContainer));
            return;
        }
        const auto addCard = [this](const QString& title, const QString& value,
                                    const std::optional<QString>& email = std::nullopt,
                                    const QString& name = QString{})
        {
            auto* card = new QWidget(m_cardContainer);
            card->setObjectName(QStringLiteral("contactCard"));
            auto* layout = new QVBoxLayout(card);
            auto* header = new QHBoxLayout();
            auto* heading = new QLabel(title, card);
            heading->setObjectName(QStringLiteral("contactCardTitle"));
            header->addWidget(heading);
            header->addStretch(1);
            if (email.has_value())
            {
                auto* compose = new QToolButton(card);
                compose->setIcon(javelin::gui::themedSvgIcon(
                    QStringLiteral(":/icons/thunderbird-icons/new-mail.svg"),
                    compose->palette().color(QPalette::ButtonText)));
                compose->setToolTip(QStringLiteral("Compose mail"));
                connect(compose, &QToolButton::clicked, this,
                        [this, email = *email, name]
                        {
                            Q_EMIT composeMailRequested(m_accountCombo->currentData().toString(),
                                                        name, email);
                        });
                header->addWidget(compose);
                auto* search = new QToolButton(card);
                search->setIcon(QIcon::fromTheme(QStringLiteral("edit-find")));
                search->setToolTip(QStringLiteral("Find mail from this address"));
                search->setAccessibleName(search->toolTip());
                connect(search, &QToolButton::clicked, this,
                        [this, email = *email]
                        {
                            Q_EMIT searchMailFromRequested(m_accountCombo->currentData().toString(),
                                                           email);
                        });
                header->addWidget(search);
            }
            auto* copy = new QToolButton(card);
            copy->setText(QStringLiteral("Copy"));
            copy->setToolTip(QStringLiteral("Copy to clipboard"));
            connect(copy, &QToolButton::clicked, card,
                    [value] { QApplication::clipboard()->setText(value); });
            header->addWidget(copy);
            layout->addLayout(header);
            auto* content = new QLabel(value, card);
            content->setTextInteractionFlags(Qt::TextSelectableByMouse);
            content->setWordWrap(true);
            layout->addWidget(content);
            m_cardLayout->addWidget(card);
        };
        if (!editorData->organization.empty() || !editorData->title.empty())
        {
            QStringList identity;
            if (!editorData->organization.empty())
                identity << QString::fromStdString(editorData->organization);
            if (!editorData->title.empty())
                identity << QString::fromStdString(editorData->title);
            addCard(QStringLiteral("Work"), identity.join(QLatin1Char('\n')));
        }
        const auto fieldTitle =
            [](const QString& fallback, const javelin::jmap::contacts::ContactEditorField& field)
        {
            if (field.label.has_value() && !field.label->empty())
                return QString::fromStdString(*field.label);
            if (const auto work = field.contexts.find("work");
                work != field.contexts.end() && work->second)
                return QStringLiteral("%1 · Work").arg(fallback);
            if (const auto home = field.contexts.find("private");
                home != field.contexts.end() && home->second)
                return QStringLiteral("%1 · Home").arg(fallback);
            return fallback;
        };
        for (const auto& email : editorData->emails)
            addCard(fieldTitle(QStringLiteral("Email"), email), QString::fromStdString(email.value),
                    QString::fromStdString(email.value),
                    QString::fromStdString(editorData->fullName));
        for (const auto& phone : editorData->phones)
            addCard(fieldTitle(QStringLiteral("Phone"), phone),
                    QString::fromStdString(phone.value));
        for (const auto& address : editorData->addresses)
            addCard(fieldTitle(QStringLiteral("Address"), address),
                    QString::fromStdString(address.value));
        if (!editorData->birthday.empty())
            addCard(QStringLiteral("Birthday"), QString::fromStdString(editorData->birthday));
        if (!editorData->notes.empty())
            addCard(QStringLiteral("Notes"), QString::fromStdString(editorData->notes));
        m_cardLayout->addStretch(1);
    }
} // namespace javelin::gui::contacts
