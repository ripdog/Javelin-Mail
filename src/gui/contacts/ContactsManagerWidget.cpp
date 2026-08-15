#include "gui/contacts/ContactsManagerWidget.h"
#include "gui/contacts/AddressBookController.h"
#include "gui/contacts/ContactDetailsView.h"
#include "gui/contacts/ContactEditor.h"
#include "gui/contacts/ContactGroupController.h"
#include "gui/contacts/ContactPhotoController.h"
#include "gui/contacts/ContactsBrowser.h"
#include "gui/widgets/EmailAddressLineEdit.h"

#include "gui/FontUtils.h"
#include "gui/IconUtils.h"
#include "gui/settings/GuiSettings.h"
#include "jmap/contacts/ContactInterchange.h"
#include "jmap/contacts/ContactResults.h"

#include <QCoroTask>

#include <KLocalizedString>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateEdit>
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
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
            AddressBook = 5,
            AccountStarred = 6,
            NoSubscribedAddressBooks = 7,
        };

        constexpr int groupFilterModeRole = Qt::UserRole + 10;
        constexpr int groupIdRole = Qt::UserRole + 11;
        constexpr int contactAccountIdRole = Qt::UserRole + 12;
        constexpr int contactUidRole = Qt::UserRole + 13;
        constexpr int stableItemKeyRole = Qt::UserRole + 14;
        constexpr int groupAccountIdRole = Qt::UserRole + 15;
        constexpr int groupAddressBookIdRole = Qt::UserRole + 16;

        [[nodiscard]] std::string contactSelectionKey(const std::string_view accountId,
                                                      const std::string_view contactId)
        {
            return std::string{accountId} + '\n' + std::string{contactId};
        }

        struct ComboEntry
        {
            QString label;
            QString id;
        };

        void mergeComboEntries(QComboBox& combo, const std::vector<ComboEntry>& desired,
                               const QString& selectedId)
        {
            QSignalBlocker blocker{&combo};
            for (int row = combo.count() - 1; row >= 0; --row)
            {
                const auto found =
                    std::ranges::find(desired, combo.itemData(row).toString(), &ComboEntry::id);
                if (found == desired.end())
                    combo.removeItem(row);
            }
            for (std::size_t index = 0; index < desired.size(); ++index)
            {
                const auto& entry = desired[index];
                int row = combo.findData(entry.id);
                const auto target = static_cast<int>(index);
                if (row < 0)
                {
                    combo.insertItem(target, entry.label, entry.id);
                    row = target;
                }
                else if (row != target)
                {
                    combo.removeItem(row);
                    combo.insertItem(target, entry.label, entry.id);
                    row = target;
                }
                combo.setItemText(row, entry.label);
            }
            const int selected = combo.findData(selectedId);
            combo.setCurrentIndex(selected >= 0 ? selected : (combo.count() == 0 ? -1 : 0));
        }

        template <typename Update>
        void mergeListItems(QListWidget& list, const std::vector<QString>& desiredKeys,
                            Update&& update)
        {
            for (std::size_t index = 0; index < desiredKeys.size(); ++index)
            {
                const int target = static_cast<int>(index);
                int existing = -1;
                for (int row = target; row < list.count(); ++row)
                {
                    if (list.item(row)->data(stableItemKeyRole).toString() == desiredKeys[index])
                    {
                        existing = row;
                        break;
                    }
                }
                QListWidgetItem* item = nullptr;
                if (existing < 0)
                {
                    item = new QListWidgetItem();
                    list.insertItem(target, item);
                }
                else if (existing != target)
                {
                    item = list.takeItem(existing);
                    list.insertItem(target, item);
                }
                else
                    item = list.item(target);
                item->setData(stableItemKeyRole, desiredKeys[index]);
                std::invoke(std::forward<Update>(update), *item, index);
            }
            while (list.count() > static_cast<int>(desiredKeys.size()))
                delete list.takeItem(list.count() - 1);
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

    class BirthdayPresenceCheckBox final : public QCheckBox
    {
      public:
        using QCheckBox::QCheckBox;

      protected:
        void nextCheckState() override
        {
            setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        }
    };

    class BirthdayEditor final : public QWidget
    {
      public:
        explicit BirthdayEditor(QWidget* parent) : QWidget(parent)
        {
            setObjectName(QStringLiteral("contactsBirthdayEditor"));
            auto* layout = new QHBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(6);
            m_enabled = new BirthdayPresenceCheckBox(i18nc("@option:check", "Set"), this);
            m_enabled->setTristate(true);
            m_enabled->setToolTip(i18n("Enable or clear the birthday"));
            m_date = new QDateEdit(QDate::currentDate(), this);
            m_date->setObjectName(QStringLiteral("contactsBirthdayEdit"));
            m_date->setCalendarPopup(true);
            m_date->setKeyboardTracking(false);
            m_date->setMaximumDate(QDate::currentDate());
            m_date->setEnabled(false);
            layout->addWidget(m_enabled);
            layout->addWidget(m_date, 1);
            connect(m_enabled, &QCheckBox::checkStateChanged, this,
                    [this](const Qt::CheckState state)
                    {
                        if (m_loading)
                            return;
                        m_preservedBirthday.clear();
                        m_date->setEnabled(state == Qt::Checked);
                        m_enabled->setToolTip(i18n("Enable or clear the birthday"));
                    });
        }

        void setBirthday(const std::string_view birthday)
        {
            m_loading = true;
            m_preservedBirthday.clear();
            const QString value =
                QString::fromUtf8(birthday.data(), static_cast<qsizetype>(birthday.size()));
            const QDate date = QDate::fromString(value, Qt::ISODate);
            if (birthday.empty())
            {
                m_enabled->setCheckState(Qt::Unchecked);
                m_date->setDate(QDate::currentDate());
                m_date->setEnabled(false);
                m_enabled->setToolTip(i18n("Enable or clear the birthday"));
            }
            else if (date.isValid() && value.size() == 10)
            {
                m_enabled->setCheckState(Qt::Checked);
                m_date->setDate(date);
                m_date->setEnabled(true);
                m_enabled->setToolTip(i18n("Enable or clear the birthday"));
            }
            else
            {
                m_preservedBirthday = value;
                m_enabled->setCheckState(Qt::PartiallyChecked);
                m_date->setDate(QDate::currentDate());
                m_date->setEnabled(false);
                m_enabled->setToolTip(
                    i18n("A partial birthday is preserved in the advanced contact document."));
            }
            m_loading = false;
        }

        [[nodiscard]] std::string birthday() const
        {
            if (m_enabled->checkState() == Qt::PartiallyChecked)
                return m_preservedBirthday.toStdString();
            if (m_enabled->checkState() != Qt::Checked)
                return {};
            return m_date->date().toString(Qt::ISODate).toStdString();
        }

      private:
        QCheckBox* m_enabled = nullptr;
        QDateEdit* m_date = nullptr;
        QString m_preservedBirthday;
        bool m_loading = false;
    };

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
                m_value = new javelin::gui::widgets::EmailAddressLineEdit(value, false, this);
            else
                m_value = new QLineEdit(value, this);
            m_value->setObjectName(QStringLiteral("contactFieldValue"));
            m_value->setAccessibleName(placeholder);
            m_value->setPlaceholderText(placeholder);
            m_context = new QComboBox(this);
            m_context->setObjectName(QStringLiteral("contactFieldType"));
            m_context->setAccessibleName(i18n("%1 type", placeholder));
            m_context->addItem(i18nc("@item contact field context", "Other"), QStringLiteral(""));
            m_context->addItem(i18nc("@item contact field context", "Home"),
                               QStringLiteral("private"));
            m_context->addItem(i18nc("@item contact field context", "Work"),
                               QStringLiteral("work"));
            const auto activeContexts = std::ranges::count_if(
                m_original.contexts, [](const auto& context) { return context.second; });
            if (activeContexts == 1)
            {
                const auto active = std::ranges::find_if(
                    m_original.contexts, [](const auto& context) { return context.second; });
                const int index = active == m_original.contexts.end()
                                      ? -1
                                      : m_context->findData(QString::fromStdString(active->first));
                if (index >= 0)
                    m_context->setCurrentIndex(index);
                else
                    addPreservedContext();
            }
            else if (activeContexts > 0)
                addPreservedContext();
            m_label = new QLineEdit(m_original.label.has_value()
                                        ? QString::fromStdString(*m_original.label)
                                        : QString{},
                                    this);
            m_label->setObjectName(QStringLiteral("contactFieldLabel"));
            m_label->setAccessibleName(i18n("%1 custom label", placeholder));
            m_label->setPlaceholderText(i18nc("@info:placeholder contact field label", "Label"));
            m_moveUp = new QToolButton(this);
            m_moveUp->setObjectName(QStringLiteral("contactFieldMoveUp"));
            m_moveUp->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
            m_moveUp->setAccessibleName(i18n("Move %1 field up", placeholder));
            m_moveUp->setToolTip(i18n("Move field up"));
            m_moveDown = new QToolButton(this);
            m_moveDown->setObjectName(QStringLiteral("contactFieldMoveDown"));
            m_moveDown->setIcon(QIcon::fromTheme(QStringLiteral("go-down")));
            m_moveDown->setAccessibleName(i18n("Move %1 field down", placeholder));
            m_moveDown->setToolTip(i18n("Move field down"));
            m_remove = new QToolButton(this);
            m_remove->setIcon(QIcon::fromTheme(QStringLiteral("list-remove")));
            m_remove->setAccessibleName(i18n("Remove %1 field", placeholder));
            m_remove->setToolTip(i18n("Remove field"));
            layout->addWidget(m_value, 1);
            layout->addWidget(m_context);
            layout->addWidget(m_label);
            layout->addWidget(m_moveUp);
            layout->addWidget(m_moveDown);
            layout->addWidget(m_remove);
            updateLabelVisibility();
            connect(m_context, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this]
                    {
                        m_contextChanged = true;
                        updateLabelVisibility();
                    });
        }

        [[nodiscard]] javelin::jmap::contacts::ContactEditorField field() const
        {
            auto result = m_original;
            result.value = m_value->text().trimmed().toStdString();
            result.preference.reset();
            const QString context = m_context->currentData().toString();
            if (context.isEmpty())
            {
                const auto label = m_label->text().trimmed();
                result.label = label.isEmpty() ? std::nullopt
                                               : std::optional<std::string>{label.toStdString()};
            }
            else if (context != QStringLiteral("__preserve__") && m_contextChanged)
                result.label.reset();
            if (context != QStringLiteral("__preserve__"))
            {
                result.contexts.clear();
                if (!context.isEmpty())
                    result.contexts.emplace(context.toStdString(), true);
            }
            return result;
        }

        [[nodiscard]] QLineEdit* valueEdit() const
        {
            return m_value;
        }

        [[nodiscard]] QToolButton* moveUpButton() const
        {
            return m_moveUp;
        }

        [[nodiscard]] QToolButton* moveDownButton() const
        {
            return m_moveDown;
        }

        [[nodiscard]] QToolButton* removeButton() const
        {
            return m_remove;
        }

        void setMoveEnabled(const bool up, const bool down)
        {
            m_moveUp->setEnabled(up);
            m_moveDown->setEnabled(down);
        }

      private:
        void addPreservedContext()
        {
            m_context->addItem(i18nc("@item contact field context", "Custom (preserved)"),
                               QStringLiteral("__preserve__"));
            m_context->setCurrentIndex(m_context->count() - 1);
        }

        void updateLabelVisibility()
        {
            m_label->setVisible(m_context->currentData().toString().isEmpty());
        }

        javelin::jmap::contacts::ContactEditorField m_original;
        QLineEdit* m_value = nullptr;
        QComboBox* m_context = nullptr;
        QLineEdit* m_label = nullptr;
        QToolButton* m_moveUp = nullptr;
        QToolButton* m_moveDown = nullptr;
        QToolButton* m_remove = nullptr;
        bool m_contextChanged = false;
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
            headers->addWidget(new QLabel(i18nc("@title contact field column", "Value"), this), 1);
            headers->addWidget(new QLabel(i18nc("@title contact field column", "Type"), this));
            headers->addWidget(
                new QLabel(i18nc("@title contact field column", "Other label"), this));
            headers->addWidget(new QLabel(i18nc("@title contact field column", "Order"), this));
            headers->addSpacing(48);
            layout->addLayout(headers);
            m_rows = new QVBoxLayout();
            m_rows->setContentsMargins(0, 0, 0, 0);
            m_rows->setSpacing(6);
            layout->addLayout(m_rows);
            auto* add = new QPushButton(i18nc("@action:button", "Add"), this);
            add->setObjectName(QStringLiteral("contactFieldAdd"));
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
            updateMoveButtons();
        }

        [[nodiscard]] std::vector<javelin::jmap::contacts::ContactEditorField> fields() const
        {
            std::vector<javelin::jmap::contacts::ContactEditorField> result;
            for (int index = 0; index < m_rows->count(); ++index)
            {
                const auto* row = static_cast<ContactFieldRow*>(m_rows->itemAt(index)->widget());
                auto field = row->field();
                if (field.value.empty())
                    continue;
                field.preference = static_cast<std::uint32_t>(result.size() + 1);
                result.push_back(std::move(field));
            }
            return result;
        }

      private:
        void addRow(javelin::jmap::contacts::ContactEditorField field)
        {
            auto* row =
                new ContactFieldRow(std::move(field), m_placeholder, m_emailAddresses, this);
            connect(row->moveUpButton(), &QToolButton::clicked, this,
                    [this, row] { moveRow(row, -1); });
            connect(row->moveDownButton(), &QToolButton::clicked, this,
                    [this, row] { moveRow(row, 1); });
            connect(row->removeButton(), &QToolButton::clicked, this,
                    [this, row]
                    {
                        m_rows->removeWidget(row);
                        row->deleteLater();
                        updateMoveButtons();
                    });
            m_rows->addWidget(row);
            updateMoveButtons();
            row->valueEdit()->setFocus();
        }

        void moveRow(ContactFieldRow* row, const int offset)
        {
            const int current = m_rows->indexOf(row);
            const int destination = current + offset;
            if (current < 0 || destination < 0 || destination >= m_rows->count())
                return;
            m_rows->removeWidget(row);
            m_rows->insertWidget(destination, row);
            updateMoveButtons();
        }

        void updateMoveButtons() const
        {
            for (int index = 0; index < m_rows->count(); ++index)
            {
                auto* row = static_cast<ContactFieldRow*>(m_rows->itemAt(index)->widget());
                row->setMoveEnabled(index > 0, index + 1 < m_rows->count());
            }
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
                setWindowTitle(m_value.id.empty() ? i18n("New Address Book")
                                                  : i18n("Edit Address Book"));
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
                form->addRow(i18n("Name"), m_name);
                form->addRow(i18n("Description"), m_description);
                form->addRow(i18n("Sort order"), m_sortOrder);
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
                                    this, i18n("Invalid Address Book"),
                                    i18n("The name must contain 1 to 255 UTF-8 bytes."));
                                return;
                            }
                            m_value.name = m_name->text().trimmed().toStdString();
                            m_value.description = m_description->text().isEmpty()
                                                      ? std::nullopt
                                                      : std::optional<std::string>{
                                                            m_description->text().toStdString()};
                            m_value.sortOrder = static_cast<std::uint32_t>(m_sortOrder->value());
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
        };

        [[nodiscard]] QString accountLabel(const javelin::gui::settings::GuiSettings& guiSettings,
                                           const javelin::jmap::cache::ContactAccount& account)
        {
            const auto settings =
                guiSettings.accountForCachedId(QString::fromStdString(account.accountId));
            if (!settings.displayName.isEmpty())
                return settings.displayName;
            return account.name.empty() ? i18n("Unnamed account")
                                        : QString::fromStdString(account.name);
        }

    } // namespace

    ContactsManagerWidget::ContactsManagerWidget(javelin::gui::settings::GuiSettings& settings,
                                                 javelin::jmap::cache::ContactReader& repository,
                                                 javelin::app::ContactRefreshPort& refreshPort,
                                                 javelin::app::ContactCommandPort& commandPort,
                                                 QWidget* parent)
        : QWidget(parent), m_settings(settings), m_repository(repository),
          m_refreshPort(refreshPort), m_commandPort(commandPort)
    {
        static_cast<void>(m_repository.connectChanged(
            this,
            [this](const QString& accountId)
            {
                if (std::ranges::contains(m_accounts, accountId.toStdString(),
                                          &javelin::jmap::cache::ContactAccount::accountId))
                    reloadAccounts();
            }));
        setupUi();
        m_browser =
            std::make_unique<ContactsBrowser>(*m_accountCombo, *m_addressBookCombo, *m_groupList,
                                              *m_contactList, m_contacts, m_groups);
        m_detailsView = std::make_unique<ContactDetailsView>(
            *m_viewTitle, *m_viewLocation, *m_starButton, *m_photoLabel, *m_editorPhotoLabel,
            *m_cardContainer, *m_cardLayout, m_repository, m_accounts,
            [this] { return m_accountCombo->currentData().toString(); },
            [this](QString accountId, QString name, QString email)
            { Q_EMIT composeMailRequested(accountId, name, email); },
            [this](QString accountId, QString email)
            { Q_EMIT searchMailFromRequested(accountId, email); });
        m_contactEditor = std::make_unique<ContactEditor>(
            *m_contactForm, *m_kindEdit, *m_organizationEdit, *m_titleEdit, *m_membersEdit,
            *m_groupContactDetailsToggle, *m_emailsEdit, *m_phonesEdit, *m_addressesEdit,
            *m_birthdayEdit);
        updateEditorKindFields();
        const auto ownerResolver = [this](const std::string_view accountId)
        { return ownerAccountId(accountId); };
        const auto busySetter = [this](const bool busy) { setBusy(busy); };
        const auto status = [this](QString message, const int timeoutMs)
        { Q_EMIT statusMessageRequested(message, timeoutMs); };
        m_groupController = std::make_unique<ContactGroupController>(
            m_commandPort, *this, ownerResolver, busySetter, status);
        m_addressBookController = std::make_unique<AddressBookController>(
            m_commandPort, m_repository, *this, ownerResolver, busySetter, status);
        m_photoController = std::make_unique<ContactPhotoController>(
            m_commandPort, *this, ownerResolver, busySetter, status);
        reloadAccounts();
    }

    ContactsManagerWidget::~ContactsManagerWidget() = default;

    void ContactsManagerWidget::applicationPaletteChanged()
    {
        const auto highlight =
            m_contactList->palette().color(QPalette::Active, QPalette::Highlight);
        for (int row = 0; row < m_contactList->count(); ++row)
        {
            auto* item = m_contactList->item(row);
            const auto contactIndex = static_cast<std::size_t>(row);
            item->setIcon(
                contactIndex < m_contacts.size() && m_contacts[contactIndex].isImportant
                    ? javelin::gui::themedSvgIcon(
                          QStringLiteral(":/icons/thunderbird-icons/starred.svg"), highlight)
                    : QIcon{});
        }
        for (int row = 0; row < m_groupList->count(); ++row)
        {
            auto* item = m_groupList->item(row);
            const auto mode = static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt());
            item->setIcon(mode == GroupFilterMode::Group
                              ? QIcon::fromTheme(QStringLiteral("system-users"))
                              : QIcon{});
        }

        const auto selected = selectedContacts();
        if (selected.size() > 1)
        {
            rebuildMultipleSelectionSummary(selected);
            return;
        }
        if (selected.empty())
        {
            return;
        }

        const auto* contact = selected.front();
        m_starButton->setIcon(javelin::gui::themedSvgIcon(
            contact->isImportant ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                                 : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
            m_starButton->palette().color(QPalette::Active, contact->isImportant
                                                                ? QPalette::Highlight
                                                                : QPalette::ButtonText)));
        if (m_detailStack->currentIndex() == 1)
        {
            m_detailsView->populateCards(*contact);
        }
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

    bool ContactsManagerWidget::canEditGroup() const
    {
        const auto* group = currentGroup();
        return group != nullptr && groupIsWritable(*group);
    }

    bool ContactsManagerWidget::canDeleteGroup() const
    {
        const auto* group = currentGroup();
        return group != nullptr && groupIsWritable(*group);
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
            const auto mode = static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt());
            const bool identityMatches =
                (mode != GroupFilterMode::Group ||
                 (item->data(groupIdRole).toString().toStdString() == state.groupId &&
                  item->data(groupAccountIdRole).toString().toStdString() == state.accountId)) &&
                (mode != GroupFilterMode::AddressBook ||
                 (item->data(groupAccountIdRole).toString().toStdString() == state.accountId &&
                  item->data(groupAddressBookIdRole).toString().toStdString() ==
                      state.addressBookId)) &&
                (mode != GroupFilterMode::AccountStarred ||
                 item->data(groupAccountIdRole).toString().toStdString() == state.accountId);
            if (item->data(groupFilterModeRole).toInt() == state.groupFilterMode && identityMatches)
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
        m_accountCombo = new QComboBox(this);
        m_accountCombo->setObjectName(QStringLiteral("contactsAccountCombo"));
        m_addressBookCombo = new QComboBox(this);
        m_addressBookCombo->setObjectName(QStringLiteral("contactsAddressBookCombo"));
        m_accountCombo->hide();
        m_addressBookCombo->hide();

        auto* splitter = new QSplitter(Qt::Horizontal, this);
        auto* groups = new QWidget(splitter);
        auto* groupsLayout = new QVBoxLayout(groups);
        groupsLayout->addWidget(new QLabel(i18n("Groups"), groups));
        m_groupList = new ContactGroupList(groups);
        m_groupList->setObjectName(QStringLiteral("contactsGroupList"));
        m_groupList->setAcceptDrops(true);
        m_groupList->setDragDropMode(QAbstractItemView::DropOnly);
        m_groupList->setDefaultDropAction(Qt::CopyAction);
        m_groupList->setContextMenuPolicy(Qt::CustomContextMenu);
        m_groupList->setMinimumWidth(165);
        static_cast<ContactGroupList*>(m_groupList)->membershipDropped =
            [this](std::string groupId, std::vector<std::string> memberUids)
        { setContactGroupMembership(std::move(groupId), std::move(memberUids), true); };
        groupsLayout->addWidget(m_groupList, 1);
        auto* left = new QWidget(splitter);
        auto* leftLayout = new QVBoxLayout(left);
        auto* searchRow = new QHBoxLayout();
        m_filterEdit = new QLineEdit(left);
        m_filterEdit->setPlaceholderText(i18n("Filter contacts"));
        m_sortCombo = new QComboBox(left);
        m_sortCombo->addItem(i18nc("@item contact sort order", "Name A–Z"), 0);
        m_sortCombo->addItem(i18nc("@item contact sort order", "Name Z–A"), 1);
        m_sortCombo->addItem(i18nc("@item contact sort order", "Organization"), 2);
        m_sortCombo->addItem(i18nc("@item contact sort order", "Email"), 3);
        searchRow->addWidget(m_filterEdit, 1);
        searchRow->addWidget(m_sortCombo);
        leftLayout->addLayout(searchRow);
        m_contactList = new QListWidget(left);
        m_contactList->setObjectName(QStringLiteral("contactsContactList"));
        m_contactList->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_contactList->setContextMenuPolicy(Qt::CustomContextMenu);
        m_contactList->setDragEnabled(true);
        m_contactList->setDragDropMode(QAbstractItemView::DragOnly);
        leftLayout->addWidget(m_contactList, 1);

        m_detailStack = new QStackedWidget(splitter);
        m_detailStack->setObjectName(QStringLiteral("contactsDetailStack"));
        auto* empty = new QLabel(i18n("Select a contact to view it."), m_detailStack);
        empty->setAlignment(Qt::AlignCenter);
        m_detailStack->addWidget(empty);
        auto* view = new QWidget(m_detailStack);
        auto* viewLayout = new QVBoxLayout(view);
        m_viewTitle = new QLabel(view);
        QFont titleFont = javelin::gui::fontWithSizeDelta(m_viewTitle->font(), 5);
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
        viewAdvanced->setText(i18n("Advanced details"));
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
        auto* viewAdvancedRow = new QHBoxLayout();
        viewAdvancedRow->addWidget(viewAdvanced);
        viewAdvancedRow->addStretch(1);
        m_viewLocation = new QLabel(view);
        m_viewLocation->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        viewAdvancedRow->addWidget(m_viewLocation);
        viewLayout->addLayout(viewAdvancedRow);
        viewLayout->addWidget(fullDocument, 1);
        m_detailStack->addWidget(view);
        auto* multiple = new QWidget(m_detailStack);
        auto* multipleLayout = new QVBoxLayout(multiple);
        m_multipleSelectionTitle = new QLabel(multiple);
        QFont multipleTitleFont =
            javelin::gui::fontWithSizeDelta(m_multipleSelectionTitle->font(), 5);
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
        auto* heading = new QLabel(i18n("Contact details"), formWidget);
        QFont headingFont = javelin::gui::fontWithSizeDelta(heading->font(), 4);
        headingFont.setBold(true);
        heading->setFont(headingFont);
        formLayout->addWidget(heading);

        m_contactForm = new QFormLayout();
        m_contactForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_kindEdit = new QComboBox(formWidget);
        m_kindEdit->setObjectName(QStringLiteral("contactsKindEdit"));
        m_kindEdit->addItem(i18nc("@item contact kind", "Person"), QStringLiteral("individual"));
        m_kindEdit->addItem(i18nc("@item contact kind", "Company or Organization"),
                            QStringLiteral("org"));
        m_kindEdit->addItem(i18nc("@item contact kind", "Group"), QStringLiteral("group"));
        m_nameEdit = new QLineEdit(formWidget);
        m_nameEdit->setObjectName(QStringLiteral("contactsNameEdit"));
        m_nameEdit->setPlaceholderText(i18n("Full name"));
        m_organizationEdit = new QLineEdit(formWidget);
        m_organizationEdit->setObjectName(QStringLiteral("contactsOrganizationEdit"));
        m_organizationEdit->setPlaceholderText(i18n("Company or organization"));
        m_titleEdit = new QLineEdit(formWidget);
        m_titleEdit->setObjectName(QStringLiteral("contactsTitleEdit"));
        m_titleEdit->setPlaceholderText(i18n("Role or job title"));
        m_contactForm->addRow(i18n("Type"), m_kindEdit);
        m_contactForm->addRow(i18n("Name"), m_nameEdit);
        m_contactForm->addRow(i18n("Organization"), m_organizationEdit);
        m_contactForm->addRow(i18n("Title"), m_titleEdit);

        m_membersEdit = new QListWidget(formWidget);
        m_membersEdit->setObjectName(QStringLiteral("contactsMembersEdit"));
        m_membersEdit->setMaximumHeight(180);
        m_contactForm->addRow(i18n("Group members"), m_membersEdit);

        m_groupContactDetailsToggle = new QToolButton(formWidget);
        m_groupContactDetailsToggle->setObjectName(
            QStringLiteral("contactsGroupContactDetailsToggle"));
        m_groupContactDetailsToggle->setText(i18n("Group contact details"));
        m_groupContactDetailsToggle->setCheckable(true);
        m_groupContactDetailsToggle->setArrowType(Qt::RightArrow);
        m_groupContactDetailsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_contactForm->addRow(m_groupContactDetailsToggle);

        m_emailsEdit = new ContactFieldEditor(i18n("Email address"), true, formWidget);
        m_emailsEdit->setObjectName(QStringLiteral("contactsEmailsEdit"));
        m_phonesEdit = new ContactFieldEditor(i18n("Phone number"), false, formWidget);
        m_phonesEdit->setObjectName(QStringLiteral("contactsPhonesEdit"));
        m_addressesEdit = new ContactFieldEditor(i18n("Postal address"), false, formWidget);
        m_addressesEdit->setObjectName(QStringLiteral("contactsAddressesEdit"));
        m_contactForm->addRow(i18n("Emails"), m_emailsEdit);
        m_contactForm->addRow(i18n("Phones"), m_phonesEdit);
        m_contactForm->addRow(i18n("Addresses"), m_addressesEdit);

        m_birthdayEdit = new BirthdayEditor(formWidget);
        m_notesEdit = new QPlainTextEdit(formWidget);
        m_notesEdit->setPlaceholderText(i18n("Notes"));
        m_notesEdit->setMaximumHeight(100);
        m_addressBooksEdit = new QListWidget(formWidget);
        m_addressBooksEdit->setObjectName(QStringLiteral("contactsAddressBooksEdit"));
        m_addressBooksEdit->setMaximumHeight(110);
        m_contactForm->addRow(i18n("Birthday"), m_birthdayEdit);
        m_contactForm->addRow(i18n("Notes"), m_notesEdit);
        formLayout->addLayout(m_contactForm);

        connect(m_kindEdit, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this]
                {
                    if (m_kindEdit->currentData().toString() == QStringLiteral("group"))
                        m_groupContactDetailsToggle->setChecked(false);
                    updateEditorKindFields();
                });
        connect(m_groupContactDetailsToggle, &QToolButton::toggled, this,
                [this](const bool expanded)
                {
                    m_groupContactDetailsToggle->setArrowType(expanded ? Qt::DownArrow
                                                                       : Qt::RightArrow);
                    updateEditorKindFields();
                });

        m_advancedToggle = new QToolButton(formWidget);
        m_advancedToggle->setObjectName(QStringLiteral("contactsAdvancedToggle"));
        m_advancedToggle->setText(i18n("Advanced and unusual fields"));
        m_advancedToggle->setCheckable(true);
        m_advancedToggle->setArrowType(Qt::RightArrow);
        m_advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        m_advancedDetails = new QWidget(formWidget);
        m_advancedDetails->setObjectName(QStringLiteral("contactsAdvancedDetails"));
        auto* advancedForm = new QFormLayout(m_advancedDetails);
        advancedForm->setContentsMargins(0, 0, 0, 0);
        advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        m_documentEdit = new QPlainTextEdit(m_advancedDetails);
        m_documentEdit->setMinimumHeight(220);
        advancedForm->addRow(i18n("Address books"), m_addressBooksEdit);
        advancedForm->addRow(i18n("Contact document"), m_documentEdit);
        m_advancedDetails->setVisible(false);
        connect(m_advancedToggle, &QToolButton::toggled, m_advancedDetails,
                [this](const bool expanded)
                {
                    m_advancedToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                    m_advancedDetails->setVisible(expanded);
                });
        formLayout->addWidget(m_advancedToggle);
        formLayout->addWidget(m_advancedDetails);
        formLayout->addStretch(1);
        scroll->setWidget(formWidget);
        auto* editButtons = new QHBoxLayout();
        m_editorPhotoLabel = new QLabel(edit);
        m_editorPhotoLabel->setFixedSize(64, 64);
        m_editorPhotoLabel->setAlignment(Qt::AlignCenter);
        m_editorPhotoLabel->setVisible(false);
        editButtons->addWidget(m_editorPhotoLabel);
        m_uploadPhotoButton = new QPushButton(i18n("Replace Photo…"), edit);
        m_removePhotoButton = new QPushButton(i18n("Remove Photo"), edit);
        editButtons->addWidget(m_uploadPhotoButton);
        editButtons->addWidget(m_removePhotoButton);
        editButtons->addStretch(1);
        m_editorLocation = new QLabel(edit);
        m_editorLocation->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        editButtons->addWidget(m_editorLocation);
        m_cancelButton = new QPushButton(i18nc("@action:button", "Cancel"), edit);
        m_saveButton = new QPushButton(i18nc("@action:button", "Save"), edit);
        m_saveButton->setObjectName(QStringLiteral("contactsSaveButton"));
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
        connect(m_groupList, &QListWidget::currentRowChanged, this,
                [this]
                {
                    if (const auto accountId = currentAccountId(); accountId.has_value())
                    {
                        const auto listed = m_repository.listAddressBooks(*accountId);
                        if (const auto* books =
                                std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listed))
                            m_addressBooks = *books;
                    }
                    reloadContacts();
                });
        connect(m_groupList, &QListWidget::customContextMenuRequested, this,
                &ContactsManagerWidget::showGroupContextMenu);
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
        const QString selected = QString::fromStdString(currentAccountId().value_or(std::string{}));
        const auto result = m_repository.listAccounts();
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
        m_accounts = std::get<std::vector<javelin::jmap::cache::ContactAccount>>(result);
        std::vector<ComboEntry> entries;
        entries.reserve(m_accounts.size());
        for (const auto& account : m_accounts)
            entries.push_back({.label = accountLabel(m_settings, account),
                               .id = QString::fromStdString(account.accountId)});
        mergeComboEntries(*m_accountCombo, entries, selected);
        reloadAddressBooks();
    }

    void ContactsManagerWidget::reloadAddressBooks()
    {
        const QString selected =
            QString::fromStdString(currentAddressBookId().value_or(std::string{}));
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
        {
            m_addressBooks.clear();
            mergeComboEntries(*m_addressBookCombo, {{.label = i18n("All address books"), .id = {}}},
                              selected);
            reloadContacts();
            return;
        }
        const auto result = m_repository.listAddressBooks(*accountId, false);
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
        m_addressBooks = std::get<std::vector<javelin::jmap::api::AddressBook>>(result);
        std::vector<ComboEntry> entries{{.label = i18n("All address books"), .id = {}}};
        entries.reserve(m_addressBooks.size() + 1);
        for (const auto& book : m_addressBooks)
        {
            QString label = QString::fromStdString(book.name);
            if (book.isDefault)
            {
                label += i18nc("@item address book suffix", " (default)");
            }
            entries.push_back({.label = std::move(label), .id = QString::fromStdString(book.id)});
        }
        mergeComboEntries(*m_addressBookCombo, entries, selected);
        reloadContacts();
    }

    void ContactsManagerWidget::reloadContacts()
    {
        const bool preserveEditor = m_detailStack->currentIndex() == 3;
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
        const QString selectedGroupAccountId =
            selectedGroupItem == nullptr ? QString{}
                                         : selectedGroupItem->data(groupAccountIdRole).toString();
        const QString selectedGroupAddressBookId =
            selectedGroupItem == nullptr
                ? QString{}
                : selectedGroupItem->data(groupAddressBookIdRole).toString();
        const QString selectedId =
            m_contactList->currentItem() == nullptr
                ? QString{}
                : m_contactList->currentItem()->data(Qt::UserRole).toString();
        const QString selectedAccountId =
            m_contactList->currentItem() == nullptr
                ? QString{}
                : m_contactList->currentItem()->data(contactAccountIdRole).toString();
        QSignalBlocker contactSelectionBlocker{m_contactList};
        if (!accountId.has_value())
        {
            m_contacts.clear();
            m_groups.clear();
            {
                QSignalBlocker groupBlocker{m_groupList};
                mergeListItems(*m_groupList, {}, [](QListWidgetItem&, std::size_t) {});
            }
            mergeListItems(*m_contactList, {}, [](QListWidgetItem&, std::size_t) {});
            if (!preserveEditor)
                showSelectedContact();
            return;
        }
        std::unordered_map<std::string, std::vector<javelin::jmap::api::AddressBook>>
            subscribedAddressBooks;
        subscribedAddressBooks.reserve(m_accounts.size());
        for (const auto& contactsAccount : m_accounts)
        {
            auto listed = m_repository.listAddressBooks(contactsAccount.accountId, false);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&listed))
            {
                Q_EMIT statusMessageRequested(error->message, 10000);
                return;
            }
            subscribedAddressBooks.emplace(
                contactsAccount.accountId,
                std::get<std::vector<javelin::jmap::api::AddressBook>>(std::move(listed)));
        }
        const auto belongsToSubscribedAddressBook =
            [&subscribedAddressBooks](const javelin::jmap::contacts::ContactSummary& contact)
        {
            const auto books = subscribedAddressBooks.find(contact.accountId);
            return books != subscribedAddressBooks.end() &&
                   std::ranges::any_of(contact.addressBookIds,
                                       [&books](const auto& addressBookId)
                                       {
                                           return std::ranges::contains(
                                               books->second, addressBookId,
                                               &javelin::jmap::api::AddressBook::id);
                                       });
        };

        if (m_optimisticContact.has_value())
        {
            const auto cached =
                m_repository.findContact(m_optimisticContact->accountId, m_optimisticContact->id);
            if (const auto* found =
                    std::get_if<std::optional<javelin::jmap::contacts::ContactSummary>>(&cached);
                found != nullptr && found->has_value())
                m_optimisticContact.reset();
        }

        m_groups.clear();
        for (const auto& contactsAccount : m_accounts)
        {
            const auto unfiltered =
                m_repository.listContacts(contactsAccount.accountId, std::nullopt);
            if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&unfiltered))
            {
                Q_EMIT statusMessageRequested(error->message, 10000);
                return;
            }
            for (const auto& contact :
                 std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(unfiltered))
                if (contact.kind == "group" && belongsToSubscribedAddressBook(contact))
                    m_groups.push_back(contact);
        }
        if (m_optimisticContact.has_value() && m_optimisticContact->kind == "group" &&
            belongsToSubscribedAddressBook(*m_optimisticContact))
            m_groups.push_back(*m_optimisticContact);
        std::ranges::sort(m_groups, {}, &javelin::jmap::contacts::ContactSummary::displayName);

        {
            QSignalBlocker blocker{m_groupList};
            struct GroupRow
            {
                QString key;
                QString label;
                GroupFilterMode mode;
                QString groupId;
                QString accountId;
                QString addressBookId;
            };
            std::vector<GroupRow> rows{
                {.key = QStringLiteral("filter:all"),
                 .label = i18n("All contacts"),
                 .mode = GroupFilterMode::All,
                 .groupId = {},
                 .accountId = {},
                 .addressBookId = {}},
                {.key = QStringLiteral("filter:ungrouped"),
                 .label = i18n("No group"),
                 .mode = GroupFilterMode::Ungrouped,
                 .groupId = {},
                 .accountId = {},
                 .addressBookId = {}},
                {.key = QStringLiteral("filter:divider"),
                 .label = QStringLiteral("────────"),
                 .mode = GroupFilterMode::Divider,
                 .groupId = {},
                 .accountId = {},
                 .addressBookId = {}},
            };
            rows.reserve(rows.size() + m_groups.size() + m_accounts.size() * 2);
            for (const auto& contactsAccount : m_accounts)
            {
                rows.push_back({
                    .key = QStringLiteral("account:%1")
                               .arg(QString::fromStdString(contactsAccount.accountId)),
                    .label = accountLabel(m_settings, contactsAccount),
                    .mode = GroupFilterMode::Divider,
                    .groupId = {},
                    .accountId = QString::fromStdString(contactsAccount.accountId),
                    .addressBookId = {},
                });
                const auto& books = subscribedAddressBooks.at(contactsAccount.accountId);
                if (books.empty())
                {
                    rows.push_back({
                        .key = QStringLiteral("no-subscribed-books:%1")
                                   .arg(QString::fromStdString(contactsAccount.accountId)),
                        .label = i18n("No subscribed address books"),
                        .mode = GroupFilterMode::NoSubscribedAddressBooks,
                        .groupId = {},
                        .accountId = QString::fromStdString(contactsAccount.accountId),
                        .addressBookId = {},
                    });
                    continue;
                }
                for (const auto& book : books)
                    rows.push_back({
                        .key = QStringLiteral("book:%1:%2")
                                   .arg(QString::fromStdString(contactsAccount.accountId),
                                        QString::fromStdString(book.id)),
                        .label = QString::fromStdString(book.name),
                        .mode = GroupFilterMode::AddressBook,
                        .groupId = {},
                        .accountId = QString::fromStdString(contactsAccount.accountId),
                        .addressBookId = QString::fromStdString(book.id),
                    });
                rows.push_back({
                    .key = QStringLiteral("starred:%1")
                               .arg(QString::fromStdString(contactsAccount.accountId)),
                    .label = i18n("Starred Contacts"),
                    .mode = GroupFilterMode::AccountStarred,
                    .groupId = {},
                    .accountId = QString::fromStdString(contactsAccount.accountId),
                    .addressBookId = {},
                });
                for (const auto& group : m_groups)
                    if (group.accountId == contactsAccount.accountId)
                    {
                        QString label = QString::fromStdString(group.displayName);
                        if (std::ranges::any_of(m_groups,
                                                [&group](const auto& candidate)
                                                {
                                                    return candidate.accountId != group.accountId &&
                                                           candidate.displayName ==
                                                               group.displayName;
                                                }))
                            label +=
                                QStringLiteral(" - ") + accountLabel(m_settings, contactsAccount);
                        rows.push_back({
                            .key = QStringLiteral("group:%1:%2")
                                       .arg(QString::fromStdString(group.accountId),
                                            QString::fromStdString(group.id)),
                            .label = std::move(label),
                            .mode = GroupFilterMode::Group,
                            .groupId = QString::fromStdString(group.id),
                            .accountId = QString::fromStdString(group.accountId),
                            .addressBookId = {},
                        });
                    }
            }
            std::vector<QString> keys;
            keys.reserve(rows.size());
            for (const auto& row : rows)
                keys.push_back(row.key);
            mergeListItems(
                *m_groupList, keys,
                [&rows](QListWidgetItem& item, const std::size_t index)
                {
                    const auto& row = rows[index];
                    item.setText(row.label);
                    item.setData(groupFilterModeRole, static_cast<int>(row.mode));
                    item.setData(groupIdRole, row.groupId);
                    item.setData(groupAccountIdRole, row.accountId);
                    item.setData(groupAddressBookIdRole, row.addressBookId);
                    item.setFlags(row.mode == GroupFilterMode::Divider ||
                                          row.mode == GroupFilterMode::NoSubscribedAddressBooks
                                      ? Qt::NoItemFlags
                                      : Qt::ItemIsSelectable | Qt::ItemIsEnabled);
                    item.setTextAlignment(row.mode == GroupFilterMode::Divider ? Qt::AlignCenter
                                                                               : Qt::AlignLeft);
                    item.setIcon(row.mode == GroupFilterMode::Group
                                     ? QIcon::fromTheme(QStringLiteral("system-users"))
                                     : QIcon{});
                });
            QListWidgetItem* restoredGroup = nullptr;
            for (int row = 0; row < m_groupList->count(); ++row)
            {
                auto* item = m_groupList->item(row);
                const auto mode =
                    static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt());
                const bool identityMatches =
                    (mode != GroupFilterMode::Group ||
                     (item->data(groupIdRole).toString() == selectedGroupId &&
                      item->data(groupAccountIdRole).toString() == selectedGroupAccountId)) &&
                    (mode != GroupFilterMode::AddressBook ||
                     (item->data(groupAccountIdRole).toString() == selectedGroupAccountId &&
                      item->data(groupAddressBookIdRole).toString() ==
                          selectedGroupAddressBookId)) &&
                    (mode != GroupFilterMode::AccountStarred ||
                     item->data(groupAccountIdRole).toString() == selectedGroupAccountId);
                if (mode == selectedGroupMode && identityMatches)
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
        m_contacts.clear();
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
                        if (contact.kind != "group" && belongsToSubscribedAddressBook(contact) &&
                            memberUids.contains(contact.uid) &&
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
            for (const auto& contactsAccount : m_accounts)
            {
                const auto filtered = m_repository.listContacts(
                    contactsAccount.accountId,
                    activeMode == GroupFilterMode::AddressBook &&
                            contactsAccount.accountId == *currentAccountId()
                        ? std::optional<std::string_view>{*currentAddressBookId()}
                        : std::nullopt,
                    m_filterEdit->text().toStdString());
                if ((activeMode == GroupFilterMode::AddressBook ||
                     activeMode == GroupFilterMode::AccountStarred) &&
                    contactsAccount.accountId != *currentAccountId())
                    continue;
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&filtered))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    return;
                }
                for (const auto& contact :
                     std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(filtered))
                {
                    if (contact.kind != "group" && belongsToSubscribedAddressBook(contact) &&
                        ((activeMode != GroupFilterMode::Starred &&
                          activeMode != GroupFilterMode::AccountStarred) ||
                         contact.isImportant) &&
                        (activeMode != GroupFilterMode::Ungrouped ||
                         !groupedUids.contains(contact.uid)))
                        m_contacts.push_back(contact);
                }
            }
        }

        if (m_optimisticContact.has_value() && m_optimisticContact->kind != "group" &&
            belongsToSubscribedAddressBook(*m_optimisticContact))
        {
            const auto& optimistic = *m_optimisticContact;
            const QString filter = m_filterEdit->text().trimmed();
            const bool matchesFilter =
                filter.isEmpty() ||
                QString::fromStdString(optimistic.displayName)
                    .contains(filter, Qt::CaseInsensitive) ||
                (optimistic.organization.has_value() &&
                 QString::fromStdString(*optimistic.organization)
                     .contains(filter, Qt::CaseInsensitive)) ||
                std::ranges::any_of(optimistic.emails,
                                    [&filter](const auto& email)
                                    {
                                        return QString::fromStdString(email.address)
                                            .contains(filter, Qt::CaseInsensitive);
                                    });
            bool include = matchesFilter;
            if (include)
            {
                switch (activeMode)
                {
                case GroupFilterMode::All:
                    break;
                case GroupFilterMode::Starred:
                    include = optimistic.isImportant;
                    break;
                case GroupFilterMode::Group:
                {
                    include = false;
                    if (const auto* group = currentGroup())
                    {
                        const auto parsed =
                            javelin::jmap::contacts::contactEditorData(group->document);
                        if (const auto* groupData =
                                std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed))
                            include = std::ranges::contains(groupData->members, optimistic.uid);
                    }
                    break;
                }
                case GroupFilterMode::Ungrouped:
                    include = std::ranges::none_of(
                        m_groups,
                        [&optimistic](const auto& group)
                        {
                            const auto parsed =
                                javelin::jmap::contacts::contactEditorData(group.document);
                            const auto* groupData =
                                std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
                            return groupData != nullptr &&
                                   std::ranges::contains(groupData->members, optimistic.uid);
                        });
                    break;
                case GroupFilterMode::AddressBook:
                {
                    const auto selectedBook = currentAddressBookId();
                    include = optimistic.accountId == *accountId && selectedBook.has_value() &&
                              std::ranges::contains(optimistic.addressBookIds, *selectedBook);
                    break;
                }
                case GroupFilterMode::AccountStarred:
                    include = optimistic.accountId == *accountId && optimistic.isImportant;
                    break;
                case GroupFilterMode::Divider:
                case GroupFilterMode::NoSubscribedAddressBooks:
                    include = false;
                    break;
                }
            }
            if (include && std::ranges::none_of(m_contacts,
                                                [&optimistic](const auto& contact)
                                                {
                                                    return contact.accountId ==
                                                               optimistic.accountId &&
                                                           contact.id == optimistic.id;
                                                }))
                m_contacts.push_back(optimistic);
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
        std::vector<QString> contactKeys;
        contactKeys.reserve(m_contacts.size());
        for (const auto& contact : m_contacts)
            contactKeys.push_back(
                QString::fromStdString(contactSelectionKey(contact.accountId, contact.id)));
        mergeListItems(
            *m_contactList, contactKeys,
            [this, &selectedContactKeys](QListWidgetItem& item, const std::size_t index)
            {
                const auto& contact = m_contacts[index];
                QString label = QString::fromStdString(contact.displayName);
                if (std::ranges::any_of(m_contacts,
                                        [&contact](const auto& candidate)
                                        {
                                            return candidate.accountId != contact.accountId &&
                                                   candidate.displayName == contact.displayName;
                                        }))
                {
                    const auto account =
                        std::ranges::find(m_accounts, contact.accountId,
                                          &javelin::jmap::cache::ContactAccount::accountId);
                    if (account != m_accounts.end())
                        label += QStringLiteral(" - ") + accountLabel(m_settings, *account);
                }
                item.setText(label);
                item.setIcon(
                    contact.isImportant
                        ? javelin::gui::themedSvgIcon(
                              QStringLiteral(":/icons/thunderbird-icons/starred.svg"),
                              m_contactList->palette().color(QPalette::Active, QPalette::Highlight))
                        : QIcon{});
                QString detail;
                if (contact.organization.has_value())
                    detail = QString::fromStdString(*contact.organization);
                else if (!contact.emails.empty())
                    detail = QString::fromStdString(contact.emails.front().address);
                item.setToolTip(detail);
                item.setData(Qt::UserRole, QString::fromStdString(contact.id));
                item.setData(contactAccountIdRole, QString::fromStdString(contact.accountId));
                item.setData(contactUidRole, QString::fromStdString(contact.uid));
                item.setFlags(item.flags() | Qt::ItemIsDragEnabled);
                item.setSelected(selectedContactKeys.contains(
                    contactSelectionKey(contact.accountId, contact.id)));
            });

        QListWidgetItem* firstContactItem =
            m_contactList->count() == 0 ? nullptr : m_contactList->item(0);
        QListWidgetItem* selectedItem = nullptr;
        QListWidgetItem* lastRestoredSelection = nullptr;
        for (int row = 0; row < m_contactList->count(); ++row)
        {
            auto* item = m_contactList->item(row);
            if (item->data(Qt::UserRole).toString() == selectedId &&
                item->data(contactAccountIdRole).toString() == selectedAccountId)
                selectedItem = item;
            if (item->isSelected())
                lastRestoredSelection = item;
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
        if (!preserveEditor)
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
        showReadOnlyContact(*contact, true);
    }

    void ContactsManagerWidget::showReadOnlyContact(
        const javelin::jmap::contacts::ContactSummary& contact, const bool contactActions)
    {
        const auto listedBooks = m_repository.listAddressBooks(contact.accountId);
        if (const auto* books =
                std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks))
            m_addressBooks = *books;
        const bool isGroup = contact.kind == "group";
        m_detailsView->showIdentity(contact, contactLocationLabel(contact),
                                    !isGroup && contactActions && !m_busy && canEditContact());
        m_starButton->setVisible(!isGroup);
        m_starButton->setIcon(javelin::gui::themedSvgIcon(
            contact.isImportant ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                                : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
            m_starButton->palette().color(QPalette::Active, contact.isImportant
                                                                ? QPalette::Highlight
                                                                : QPalette::ButtonText)));
        m_starButton->setToolTip(contact.isImportant ? i18n("Remove from Starred")
                                                     : i18n("Add to Starred"));
        m_starButton->setAccessibleName(m_starButton->toolTip());
        m_starButton->setEnabled(!isGroup && contactActions && !m_busy && canEditContact());
        m_detailsView->populateCards(contact);
        showContactPhoto(contact);
        if (auto* document = m_detailStack->widget(1)->findChild<QPlainTextEdit*>(
                QStringLiteral("contactDocumentView")))
            document->setPlainText(QString::fromStdString(contact.document));
        m_detailStack->setCurrentIndex(1);
        Q_EMIT toolbarStateChanged(m_busy, contactActions);
    }

    void ContactsManagerWidget::showSavedContact(std::string accountId, std::string contactId,
                                                 std::string kind, std::string document)
    {
        const auto preview = javelin::jmap::contacts::summarizeContact(
            accountId,
            javelin::jmap::api::ContactCard{
                .id = contactId, .uid = {}, .kind = {}, .document = std::move(document)});
        const auto cachedBeforeReload = m_repository.findContact(accountId, contactId);
        const auto* cachedContact =
            std::get_if<std::optional<javelin::jmap::contacts::ContactSummary>>(
                &cachedBeforeReload);
        if (preview.has_value() && (cachedContact == nullptr || !cachedContact->has_value()))
            m_optimisticContact = *preview;

        m_detailStack->setCurrentIndex(0);
        reloadContacts();

        bool savedItemSelected = false;
        if (kind == "group")
        {
            for (int row = 0; row < m_groupList->count(); ++row)
            {
                auto* item = m_groupList->item(row);
                if (item->data(groupIdRole).toString().toStdString() == contactId &&
                    item->data(groupAccountIdRole).toString().toStdString() == accountId)
                {
                    m_groupList->setCurrentItem(item);
                    savedItemSelected = true;
                    break;
                }
            }
        }
        else
        {
            for (int row = 0; row < m_contactList->count(); ++row)
            {
                auto* item = m_contactList->item(row);
                if (item->data(Qt::UserRole).toString().toStdString() == contactId &&
                    item->data(contactAccountIdRole).toString().toStdString() == accountId)
                {
                    m_contactList->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
                    savedItemSelected = true;
                    break;
                }
            }
        }

        if (preview.has_value())
        {
            showReadOnlyContact(*preview, kind != "group" && savedItemSelected);
            return;
        }
        if (savedItemSelected)
        {
            if (kind != "group")
                showSelectedContact();
            return;
        }

        const auto cached = m_repository.findContact(accountId, contactId);
        if (const auto* found =
                std::get_if<std::optional<javelin::jmap::contacts::ContactSummary>>(&cached);
            found != nullptr && found->has_value())
            showReadOnlyContact(**found, kind != "group" && currentContact() != nullptr &&
                                             currentContact()->id == contactId &&
                                             currentContact()->accountId == accountId);
        else
            showSelectedContact();
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
            i18np("%1 contact selected", "%1 contacts selected", contacts.size()));
        const bool allStarred =
            std::ranges::all_of(contacts, [](const auto* contact) { return contact->isImportant; });
        m_multipleStarButton->setText(allStarred ? i18n("Remove all from Starred")
                                                 : i18n("Add all to Starred"));
        m_multipleStarButton->setIcon(javelin::gui::themedSvgIcon(
            allStarred ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                       : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
            m_multipleStarButton->palette().color(
                QPalette::Active, allStarred ? QPalette::Highlight : QPalette::ButtonText)));
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
                    details.push_back(i18nc("@item additional contact email addresses", "+%1 more",
                                            contact->emails.size() - 1));
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
            auto* menu = new QMenu{this};
            menu->setAttribute(Qt::WA_DeleteOnClose);
            const bool allStarred = std::ranges::all_of(contacts, [](const auto* selected)
                                                        { return selected->isImportant; });
            auto* starred = menu->addAction(
                javelin::gui::themedSvgIcon(
                    allStarred ? QStringLiteral(":/icons/thunderbird-icons/starred.svg")
                               : QStringLiteral(":/icons/thunderbird-icons/star.svg"),
                    m_contactList->palette().color(allStarred ? QPalette::Highlight
                                                              : QPalette::Text)),
                allStarred ? i18n("Remove all from Starred") : i18n("Add all to Starred"));
            starred->setEnabled(!m_busy && canStarSelectedContacts());
            connect(starred, &QAction::triggered, this,
                    &ContactsManagerWidget::toggleContactStarred);
            auto* addToGroup =
                menu->addMenu(QIcon::fromTheme(QStringLiteral("list-add")), i18n("Add to Group"));
            populateAddToGroupMenu(*addToGroup);
            auto* removeFromGroup = menu->addMenu(QIcon::fromTheme(QStringLiteral("list-remove")),
                                                  i18n("Remove from Group"));
            populateRemoveFromGroupMenu(*removeFromGroup);
            menu->popup(m_contactList->viewport()->mapToGlobal(position));
            return;
        }
        const auto* contact = currentContact();
        if (contact == nullptr)
            return;

        auto* menu = new QMenu{this};
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* compose = menu->addAction(QIcon::fromTheme(QStringLiteral("mail-message-new")),
                                        i18n("Write Message"));
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
            auto* copyEmail = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                              i18n("Copy Email Address"));
            connect(copyEmail, &QAction::triggered, this,
                    [email] { QApplication::clipboard()->setText(email); });
        }
        if (!contact->emails.empty())
        {
            auto* searchMenu = menu->addMenu(QIcon::fromTheme(QStringLiteral("edit-find")),
                                             i18n("Find Mail From"));
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
        auto* starred = menu->addAction(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/starred.svg"),
                                        m_contactList->palette().color(QPalette::Highlight)),
            contact->isImportant ? i18n("Remove from Starred") : i18n("Add to Starred"));
        starred->setEnabled(!m_busy && canEditContact());
        connect(starred, &QAction::triggered, this, &ContactsManagerWidget::toggleContactStarred);
        auto* addToGroup =
            menu->addMenu(QIcon::fromTheme(QStringLiteral("list-add")), i18n("Add to Group"));
        populateAddToGroupMenu(*addToGroup);
        auto* removeFromGroup = menu->addMenu(QIcon::fromTheme(QStringLiteral("list-remove")),
                                              i18n("Remove from Group"));
        populateRemoveFromGroupMenu(*removeFromGroup);
        menu->addSeparator();
        auto* edit = menu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                     i18n("Edit Contact"));
        edit->setEnabled(!m_busy && canEditContact());
        connect(edit, &QAction::triggered, this, &ContactsManagerWidget::beginEditContact);
        auto* copy =
            menu->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Contact…"));
        connect(copy, &QAction::triggered, this, &ContactsManagerWidget::copyContact);
        auto* exportAction = menu->addAction(QIcon::fromTheme(QStringLiteral("document-export")),
                                             i18n("Export vCard…"));
        connect(exportAction, &QAction::triggered, this, &ContactsManagerWidget::exportVCard);
        auto* merge = menu->addAction(QIcon::fromTheme(QStringLiteral("merge")),
                                      i18n("Find and Merge Duplicates…"));
        connect(merge, &QAction::triggered, this, &ContactsManagerWidget::findAndMergeDuplicates);
        auto* remove = menu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                       i18n("Delete Contact"));
        connect(remove, &QAction::triggered, this, &ContactsManagerWidget::deleteContact);
        copy->setEnabled(!m_busy);
        exportAction->setEnabled(!m_busy);
        merge->setEnabled(!m_busy && canEditContact());
        remove->setEnabled(!m_busy && canDeleteContact());
        menu->popup(m_contactList->viewport()->mapToGlobal(position));
    }

    void ContactsManagerWidget::showGroupContextMenu(const QPoint& position)
    {
        auto* item = m_groupList->itemAt(position);
        if (item == nullptr ||
            static_cast<GroupFilterMode>(item->data(groupFilterModeRole).toInt()) !=
                GroupFilterMode::Group)
            return;
        m_groupList->setCurrentItem(item);
        const auto* group = currentGroup();
        if (group == nullptr)
            return;

        auto* menu = new QMenu{this};
        menu->setAttribute(Qt::WA_DeleteOnClose);
        auto* edit =
            menu->addAction(QIcon::fromTheme(QStringLiteral("document-edit")), i18n("Edit Group"));
        edit->setEnabled(!m_busy && canEditGroup());
        connect(edit, &QAction::triggered, this, &ContactsManagerWidget::beginEditGroup);
        auto* remove =
            menu->addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), i18n("Delete Group"));
        remove->setEnabled(!m_busy && canDeleteGroup());
        connect(remove, &QAction::triggered, this, &ContactsManagerWidget::deleteContactGroup);
        menu->popup(m_groupList->viewport()->mapToGlobal(position));
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
            auto* unavailable = menu.addAction(i18n("No available groups"));
            unavailable->setEnabled(false);
        }
        menu.addSeparator();
        auto* create =
            menu.addAction(QIcon::fromTheme(QStringLiteral("list-add")), i18n("New Group…"));
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
            auto* unavailable = menu.addAction(i18n("Not in a writable group"));
            unavailable->setEnabled(false);
        }
    }

    void ContactsManagerWidget::populateAddressBookMenu(QMenu& menu)
    {
        menu.clear();
        for (const auto& account : m_accounts)
        {
            auto* heading = menu.addSection(accountLabel(m_settings, account));
            heading->setEnabled(false);
            const auto listed = m_repository.listAddressBooks(account.accountId);
            const auto* books = std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listed);
            if (books == nullptr)
                continue;
            for (const auto& book : *books)
            {
                auto* action = menu.addAction(QString::fromStdString(book.name));
                action->setCheckable(true);
                action->setChecked(book.isSubscribed);
                const bool canToggle =
                    canSetAddressBookSubscription(account.accountId, book, !book.isSubscribed);
                action->setEnabled(!m_busy && canToggle);
                if (!canToggle && book.isSubscribed)
                    action->setToolTip(
                        i18n("The only address book in an account cannot be unsubscribed."));
                connect(action, &QAction::toggled, this,
                        [this, accountId = account.accountId, book](const bool subscribed)
                        { setAddressBookSubscription(accountId, book, subscribed); });
            }
        }
        menu.addSeparator();
        auto* manage = menu.addAction(QIcon::fromTheme(QStringLiteral("view-list-details")),
                                      i18n("Manage Address Books…"));
        connect(manage, &QAction::triggered, this, &ContactsManagerWidget::showAddressBookManager);
    }

    void ContactsManagerWidget::beginCreateGroup()
    {
        if (m_busy || !canCreateGroup())
            return;
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        const auto listedBooks = m_repository.listAddressBooks(*accountId);
        const auto* accountBooks =
            std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks);
        if (accountBooks == nullptr)
            return;
        m_addressBooks = *accountBooks;

        std::vector<const javelin::jmap::api::AddressBook*> writableBooks;
        for (const auto& book : m_addressBooks)
            if (book.myRights.mayWrite)
                writableBooks.push_back(&book);
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
            bool accepted = false;
            QStringList labels;
            for (const auto* book : writableBooks)
                labels.push_back(QString::fromStdString(book->name));
            const QString selected = QInputDialog::getItem(
                this, i18n("New Contact Group"), i18n("Address book"), labels, 0, false, &accepted);
            const auto index = labels.indexOf(selected);
            if (!accepted || index < 0)
                return;
            target = writableBooks[static_cast<std::size_t>(index)];
        }

        const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString document =
            QStringLiteral("{\n  \"uid\": \"%1\",\n  \"kind\": \"group\",\n  "
                           "\"addressBookIds\": {\"%2\": true},\n  \"name\": {\"full\": "
                           "\"\"},\n  \"members\": {}\n}")
                .arg(uid, QString::fromStdString(target->id));
        m_creating = true;
        m_editingGroup = true;
        m_editingAccountId = *accountId;
        m_editingContactId.reset();
        loadEditorDocument(document);
        m_nameEdit->setFocus();
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
        m_groupController->setMembership(group->accountId, std::move(groupId),
                                         std::move(memberUids), included);
    }

    void ContactsManagerWidget::deleteContactGroup()
    {
        if (m_busy)
            return;
        const auto* group = currentGroup();
        if (group == nullptr || !groupIsWritable(*group))
            return;

        const QString groupName = group->displayName.empty()
                                      ? i18n("(unnamed group)")
                                      : QString::fromStdString(group->displayName);
        QStringList details{i18n("Group: %1", groupName)};
        const auto parsed = javelin::jmap::contacts::contactEditorData(group->document);
        if (const auto* editorData =
                std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed))
            details.push_back(i18n("Members: %1", editorData->members.size()));
        if (QMessageBox::question(
                this, i18n("Delete Contact Group"),
                i18n("Delete this contact group? The contacts in it will not be deleted.\n\n%1",
                     details.join(QLatin1Char('\n')))) != QMessageBox::Yes)
            return;

        m_groupController->deleteGroup(group->accountId, group->id);
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
        javelin::app::SetContactsStarredCommand command{
            .accountId = *accountId,
            .contacts = {},
            .starred = starred,
        };
        command.contacts.reserve(contacts.size());
        for (const auto* contact : contacts)
            command.contacts.push_back({.id = contact->id, .document = contact->document});
        const auto owner = ownerAccountId(command.accountId).value_or(std::string{});
        setBusy(true);
        auto task = m_commandPort.setContactsStarred(owner, std::move(command));
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
                       });
    }

    void ContactsManagerWidget::updateEditorKindFields()
    {
        m_contactEditor->updateKindFields();
    }

    void ContactsManagerWidget::loadEditorDocument(const QString& document)
    {
        const auto parsed = javelin::jmap::contacts::contactEditorData(document.toStdString());
        const auto* editorData = std::get_if<javelin::jmap::contacts::ContactEditorData>(&parsed);
        if (editorData == nullptr)
        {
            QMessageBox::warning(this, i18n("Contact Editor"),
                                 i18n("This contact document cannot be edited."));
            return;
        }
        int kindIndex = m_kindEdit->findData(QString::fromStdString(editorData->kind));
        if (kindIndex < 0)
            kindIndex = 0;
        m_kindEdit->setCurrentIndex(kindIndex);
        if (editorData->kind == "group")
            m_groupContactDetailsToggle->setChecked(false);
        updateEditorKindFields();
        m_nameEdit->setText(QString::fromStdString(editorData->fullName));
        m_organizationEdit->setText(QString::fromStdString(editorData->organization));
        m_titleEdit->setText(QString::fromStdString(editorData->title));
        m_emailsEdit->setFields(editorData->emails);
        m_phonesEdit->setFields(editorData->phones);
        m_addressesEdit->setFields(editorData->addresses);
        m_birthdayEdit->setBirthday(editorData->birthday);
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
                                                  accountLabel(m_settings, account)),
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
                i18n("%1 (currently unavailable)", QString::fromStdString(uid)), m_membersEdit);
            item->setData(Qt::UserRole, QString::fromStdString(uid));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        m_advancedToggle->setChecked(false);
        javelin::jmap::contacts::ContactSummary location{
            .accountId = m_editingAccountId.empty() ? currentAccountId().value_or(std::string{})
                                                    : m_editingAccountId,
            .id = {},
            .uid = {},
            .kind = editorData->kind,
            .displayName = editorData->fullName,
            .organization = std::nullopt,
            .emails = {},
            .addressBookIds = editorData->addressBookIds,
            .isImportant = false,
            .document = {},
        };
        m_editorLocation->setText(contactLocationLabel(location));
        m_detailStack->setCurrentIndex(3);
    }

    void ContactsManagerWidget::beginCreateContact()
    {
        if (m_busy || !canCreateContact())
            return;
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
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
                QMessageBox::information(this, i18n("No Address Book"),
                                         i18n("Create or select an address book first."));
                return;
            }
            bookId = defaultBook->id;
        }
        const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString document =
            QStringLiteral("{\n  \"uid\": \"%1\",\n  \"kind\": \"individual\",\n  "
                           "\"addressBookIds\": {\"%2\": true},\n  \"name\": {\"full\": "
                           "\"\"}\n}")
                .arg(uid, QString::fromStdString(*bookId));
        m_creating = true;
        m_editingGroup = false;
        m_editingAccountId = *accountId;
        m_editingContactId.reset();
        loadEditorDocument(document);
        m_nameEdit->setFocus();
    }

    void ContactsManagerWidget::beginEditContact()
    {
        if (m_busy || !canEditContact())
            return;
        const auto* contact = currentContact();
        if (contact == nullptr)
            return;
        const auto listedBooks = m_repository.listAddressBooks(contact->accountId);
        if (const auto* books =
                std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks))
            m_addressBooks = *books;
        m_creating = false;
        m_editingGroup = false;
        m_editingAccountId = contact->accountId;
        m_editingContactId = contact->id;
        loadEditorDocument(QString::fromStdString(contact->document));
    }

    void ContactsManagerWidget::beginEditGroup()
    {
        if (m_busy || !canEditGroup())
            return;
        const auto* group = currentGroup();
        if (group == nullptr)
            return;
        const auto listedBooks = m_repository.listAddressBooks(group->accountId);
        const auto* books = std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks);
        if (books == nullptr)
            return;
        m_addressBooks = *books;
        m_creating = false;
        m_editingGroup = true;
        m_editingAccountId = group->accountId;
        m_editingContactId = group->id;
        loadEditorDocument(QString::fromStdString(group->document));
    }

    void ContactsManagerWidget::cancelEdit()
    {
        const bool wasEditingGroup = m_editingGroup;
        m_creating = false;
        m_editingGroup = false;
        m_editingAccountId.clear();
        m_editingContactId.reset();
        if (wasEditingGroup)
        {
            if (const auto* group = currentGroup())
            {
                showReadOnlyContact(*group, false);
                return;
            }
        }
        showSelectedContact();
    }

    void ContactsManagerWidget::saveContact()
    {
        if (m_editingAccountId.empty())
            return;
        javelin::jmap::contacts::ContactEditorData editor;
        editor.kind = m_kindEdit->currentData().toString().toStdString();
        editor.fullName = m_nameEdit->text().trimmed().toStdString();
        editor.organization = m_organizationEdit->text().trimmed().toStdString();
        editor.title = m_titleEdit->text().trimmed().toStdString();
        editor.emails = m_emailsEdit->fields();
        editor.phones = m_phonesEdit->fields();
        editor.addresses = m_addressesEdit->fields();
        editor.birthday = m_birthdayEdit->birthday();
        editor.notes = m_notesEdit->toPlainText().trimmed().toStdString();
        editor.document = m_documentEdit->toPlainText().toStdString();
        const auto originalEditorData = javelin::jmap::contacts::contactEditorData(editor.document);
        if (const auto* original =
                std::get_if<javelin::jmap::contacts::ContactEditorData>(&originalEditorData))
            editor.uid = original->uid;
        editor.addressBookIds = m_contactEditor->checkedAddressBookIds(*m_addressBooksEdit);
        const auto account = std::ranges::find(m_accounts, m_editingAccountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() ||
            !javelin::jmap::contacts::contactActionRights(account->isReadOnly, m_addressBooks,
                                                          editor.addressBookIds)
                 .mayModify)
        {
            QMessageBox::information(
                this, i18n("Read-only Contact"),
                i18n("You do not have write permission for every selected address book."));
            return;
        }
        if (editor.kind == "group")
            editor.members = m_contactEditor->checkedMemberUids();
        if (!m_creating && !m_editingContactId.has_value())
            return;

        std::string previewDocument = editor.document;
        const auto applied = javelin::jmap::contacts::applyContactEditorData(editor, m_creating);
        if (const auto* appliedDocument = std::get_if<std::string>(&applied))
        {
            const auto prepared =
                javelin::jmap::contacts::prepareContactDocument(*appliedDocument, m_creating);
            previewDocument = std::holds_alternative<std::string>(prepared)
                                  ? std::get<std::string>(prepared)
                                  : *appliedDocument;
        }

        const std::string accountId = m_editingAccountId;
        const std::optional<std::string> contactId = m_editingContactId;
        const std::string kind = editor.kind;
        javelin::app::SaveContactCommand command{
            .accountId = accountId,
            .contactId = contactId,
            .contact = std::move(editor),
        };
        const auto owner = ownerAccountId(command.accountId).value_or(std::string{});
        setBusy(true);
        auto task = m_commandPort.saveContact(owner, std::move(command));
        QCoro::connect(
            std::move(task), this,
            [this, accountId, contactId, kind, previewDocument = std::move(previewDocument)](
                javelin::jmap::contacts::ContactMutationResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    Q_EMIT statusMessageRequested(error->message, 10000);
                    return;
                }
                const auto& summary =
                    std::get<javelin::jmap::contacts::ContactMutationSummary>(result);
                const auto savedId = contactId.has_value() ? contactId : summary.createdId;
                m_creating = false;
                m_editingGroup = false;
                m_editingAccountId.clear();
                m_editingContactId.reset();
                if (savedId.has_value())
                    showSavedContact(accountId, *savedId, kind, previewDocument);
                else
                    showSelectedContact();
            });
    }

    void ContactsManagerWidget::uploadPhoto()
    {
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        const QString path = QFileDialog::getOpenFileName(
            this, i18n("Choose Contact Photo"), QString{},
            i18n("Images (*.png *.jpg *.jpeg *.gif *.webp *.avif);;All Files (*)"));
        if (path.isEmpty())
            return;
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
        {
            QMessageBox::warning(this, i18n("Photo Upload"), file.errorString());
            return;
        }
        const QByteArray payload = file.readAll();
        const auto mimeType = QMimeDatabase{}.mimeTypeForFile(path, QMimeDatabase::MatchContent);
        if (!mimeType.name().startsWith(QStringLiteral("image/")))
        {
            QMessageBox::warning(this, i18n("Photo Upload"),
                                 i18n("The selected file is not a recognized image."));
            return;
        }
        m_photoController->upload(
            *accountId, payload, mimeType.name().toStdString(), [this]
            { return m_documentEdit->toPlainText().toStdString(); }, [this](std::string document)
            { m_documentEdit->setPlainText(QString::fromStdString(document)); },
            [this](const QByteArray& payloadBytes) { m_detailsView->showPhoto(payloadBytes); },
            [this](const bool enabled) { m_removePhotoButton->setEnabled(enabled); });
    }

    void ContactsManagerWidget::removePhoto()
    {
        const auto document = m_photoController->remove(
            m_documentEdit->toPlainText().toStdString(), [this] { m_detailsView->clearPhoto(); },
            [this](const bool enabled) { m_removePhotoButton->setEnabled(enabled); });
        if (const auto* error = std::get_if<QString>(&document))
        {
            QMessageBox::warning(this, i18n("Remove Photo"), *error);
            return;
        }
        m_documentEdit->setPlainText(QString::fromStdString(std::get<std::string>(document)));
    }

    void
    ContactsManagerWidget::showContactPhoto(const javelin::jmap::contacts::ContactSummary& contact)
    {
        m_photoController->show(
            contact, [this] { m_detailsView->clearPhoto(); },
            [this](const QByteArray& payloadBytes) { m_detailsView->showPhoto(payloadBytes); },
            [this](const std::string& contactId)
            {
                const auto* selected = currentContact();
                const auto* selectedGroup = currentGroup();
                return (selected != nullptr && selected->id == contactId) ||
                       (selectedGroup != nullptr && selectedGroup->id == contactId);
            });
    }

    void ContactsManagerWidget::deleteContact()
    {
        if (m_busy)
            return;
        const auto accountId = currentAccountId();
        const auto* contact = currentContact();
        if (!accountId.has_value() || contact == nullptr || !canDeleteContact())
            return;
        const QString email = contact->emails.empty()
                                  ? QString{}
                                  : QString::fromStdString(contact->emails.front().address);
        QString contactName = QString::fromStdString(contact->displayName).trimmed();
        if (contactName.isEmpty())
            contactName = email.isEmpty() ? i18n("(unnamed contact)") : email;
        QStringList details{i18n("Contact: %1", contactName)};
        if (!email.isEmpty() && email != contactName)
            details.push_back(i18n("Email: %1", email));
        QStringList addressBooks;
        for (const auto& addressBookId : contact->addressBookIds)
        {
            const auto book = std::ranges::find(m_addressBooks, addressBookId,
                                                &javelin::jmap::api::AddressBook::id);
            if (book != m_addressBooks.end())
                addressBooks.push_back(QString::fromStdString(book->name));
        }
        if (!addressBooks.isEmpty())
            details.push_back(i18n("Address books: %1", addressBooks.join(QStringLiteral(", "))));
        if (QMessageBox::question(this, i18n("Delete Contact"),
                                  i18n("Delete this contact?\n\n%1",
                                       details.join(QLatin1Char('\n')))) != QMessageBox::Yes)
            return;
        javelin::app::DeleteContactsCommand command{
            .accountId = *accountId,
            .contactIds = {contact->id},
        };
        const auto owner = ownerAccountId(command.accountId).value_or(std::string{});
        setBusy(true);
        auto task = m_commandPort.deleteContacts(owner, std::move(command));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
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
            accountLabels.push_back(accountLabel(m_settings, account));
            destinations.push_back(std::move(destination));
        }
        if (destinations.empty())
        {
            QMessageBox::information(this, i18n("Copy Contact"),
                                     i18n("There is no writable destination address book."));
            return;
        }
        bool accepted = false;
        const QString selectedAccount =
            QInputDialog::getItem(this, i18n("Copy Contact"), i18n("Destination account"),
                                  accountLabels, 0, false, &accepted);
        const qsizetype accountIndex = accountLabels.indexOf(selectedAccount);
        if (!accepted || accountIndex < 0)
            return;
        const auto& destination = destinations[static_cast<std::size_t>(accountIndex)];
        QStringList bookLabels;
        for (const auto& book : destination.books)
            bookLabels.push_back(QString::fromStdString(book.name));
        const QString selectedBook =
            QInputDialog::getItem(this, i18n("Copy Contact"), i18n("Destination address book"),
                                  bookLabels, 0, false, &accepted);
        const qsizetype bookIndex = bookLabels.indexOf(selectedBook);
        if (!accepted || bookIndex < 0)
            return;
        const auto& book = destination.books[static_cast<std::size_t>(bookIndex)];

        javelin::app::CopyContactCommand command{
            .destinationOwnerAccountId = destination.account.ownerAccountId,
            .sourceAccountId = *sourceAccountId,
            .destinationAccountId = destination.account.accountId,
            .contactId = contact->id,
            .contactDocument = contact->document,
            .destinationAddressBookId = book.id,
        };
        setBusy(true);
        auto task = m_commandPort.copyContact(
            ownerAccountId(*sourceAccountId).value_or(std::string{}), std::move(command));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
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
            QMessageBox::warning(this, i18n("Export vCard"),
                                 i18n("This contact cannot be exported."));
            return;
        }
        QString suggestedName = QString::fromStdString(contact->displayName);
        suggestedName.replace(QLatin1Char('/'), QLatin1Char('-'));
        const QString path = QFileDialog::getSaveFileName(this, i18n("Export vCard"),
                                                          suggestedName + QStringLiteral(".vcf"),
                                                          i18n("vCard files (*.vcf)"));
        if (path.isEmpty())
            return;
        QSaveFile file{path};
        if (!file.open(QIODevice::WriteOnly))
        {
            QMessageBox::warning(this, i18n("Export vCard"), file.errorString());
            return;
        }
        const auto output =
            QByteArray::fromStdString(javelin::jmap::contacts::exportVCard(*editorData));
        if (file.write(output) != output.size() || !file.commit())
        {
            QMessageBox::warning(this, i18n("Export vCard"), file.errorString());
            return;
        }
        Q_EMIT statusMessageRequested(i18n("Contact exported."), 5000);
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
            const QString selected =
                QInputDialog::getItem(this, i18n("Import vCard"), i18n("Destination address book"),
                                      labels, 0, false, &accepted);
            const auto index = labels.indexOf(selected);
            if (!accepted || index < 0)
                return;
            target = writableBooks[static_cast<std::size_t>(index)];
        }
        const QString path =
            QFileDialog::getOpenFileName(this, i18n("Import vCard"), QString{},
                                         i18n("vCard files (*.vcf *.vcard);;All files (*)"));
        if (path.isEmpty())
            return;
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly))
        {
            QMessageBox::warning(this, i18n("Import vCard"), file.errorString());
            return;
        }
        if (file.size() > 10 * 1024 * 1024)
        {
            QMessageBox::warning(this, i18n("Import vCard"),
                                 i18n("The vCard file exceeds 10 MiB."));
            return;
        }
        const auto imported = javelin::jmap::contacts::importVCards(file.readAll().toStdString());
        const auto* contacts =
            std::get_if<std::vector<javelin::jmap::contacts::ContactEditorData>>(&imported);
        if (contacts == nullptr)
        {
            const auto message = std::get<std::string_view>(imported);
            QMessageBox::warning(
                this, i18n("Import vCard"),
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
        javelin::app::ImportContactsCommand command{
            .accountId = *accountId,
            .addressBookId = target->id,
            .contacts = std::move(*contacts),
            .knownUids = std::move(knownUids),
        };
        const auto owner = ownerAccountId(command.accountId).value_or(std::string{});
        setBusy(true);
        auto task = m_commandPort.importContacts(owner, std::move(command));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
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
            QMessageBox::information(this, i18n("Duplicate Contacts"),
                                     i18n("No likely duplicates were found."));
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
            const QString selected =
                QInputDialog::getItem(this, i18n("Duplicate Contacts"), i18n("Duplicate group"),
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
        const QString primaryName = QInputDialog::getItem(this, i18n("Merge Duplicate Contacts"),
                                                          i18n("Keep this contact as the primary"),
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
                    this, i18n("Merge Duplicate Contacts"),
                    i18n("Every duplicate must be writable before it can be merged."));
                return;
            }
        }
        QStringList mergeDetails;
        for (const auto* candidate : candidates)
        {
            QString label = QString::fromStdString(candidate->displayName).trimmed();
            const QString email = candidate->emails.empty()
                                      ? QString{}
                                      : QString::fromStdString(candidate->emails.front().address);
            if (label.isEmpty())
                label = email.isEmpty() ? i18n("(unnamed contact)") : email;
            else if (!email.isEmpty())
                label = i18nc("contact confirmation name and address", "%1 <%2>", label, email);
            mergeDetails.push_back(label);
        }
        if (QMessageBox::question(
                this, i18n("Merge Duplicate Contacts"),
                i18n("Merge %1 contacts into %2? This keeps all mapped fields and removes the "
                     "redundant contacts.\n\nContacts:\n%3",
                     candidates.size(), primaryName, mergeDetails.join(QLatin1Char('\n')))) !=
            QMessageBox::Yes)
            return;
        const auto* primary = candidates[static_cast<std::size_t>(primaryIndex)];
        javelin::app::MergeContactsCommand command{
            .accountId = *accountId,
            .primary = {.id = primary->id, .document = primary->document},
            .duplicates = {},
        };
        command.duplicates.reserve(candidates.size() - 1);
        for (const auto* candidate : candidates)
        {
            if (candidate != primary)
                command.duplicates.push_back(
                    {.id = candidate->id, .document = candidate->document});
        }
        const auto owner = ownerAccountId(command.accountId).value_or(std::string{});
        setBusy(true);
        auto task = m_commandPort.mergeContacts(owner, std::move(command));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::OperationError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                       });
    }

    void ContactsManagerWidget::requestRefresh()
    {
        if (m_refreshInFlight || m_accounts.empty())
            return;
        std::unordered_set<std::string> owners;
        for (const auto& account : m_accounts)
            owners.insert(account.ownerAccountId);
        m_refreshInFlight = true;
        m_pendingRefreshes = owners.size();
        m_refreshedContacts = 0;
        m_refreshedAddressBooks = 0;
        Q_EMIT statusMessageRequested(i18n("Refreshing contacts…"), 5000);
        for (const auto& owner : owners)
        {
            auto task = m_refreshPort.requestContacts(owner);
            QCoro::connect(
                std::move(task), this,
                [this](javelin::jmap::contacts::ContactRefreshResult result)
                {
                    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        Q_EMIT statusMessageRequested(error->message, 10000);
                    else
                    {
                        const auto& summary =
                            std::get<javelin::jmap::contacts::ContactRefreshSummary>(result);
                        m_refreshedContacts += summary.contactCount;
                        m_refreshedAddressBooks += summary.addressBookCount;
                    }
                    if (--m_pendingRefreshes != 0)
                        return;
                    m_refreshInFlight = false;
                    reloadAccounts();
                    Q_EMIT statusMessageRequested(i18n("Cached %1 contacts in %2 address books.",
                                                       m_refreshedContacts,
                                                       m_refreshedAddressBooks),
                                                  5000);
                });
        }
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
        applyAddressBookMutation(
            javelin::app::CreateAddressBookCommand{
                .accountId = std::move(accountId),
                .addressBook = dialog.value(),
            },
            i18n("Creating address book…"));
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
        applyAddressBookMutation(
            javelin::app::UpdateAddressBookCommand{
                .accountId = std::move(accountId),
                .addressBook = dialog.value(),
            },
            i18n("Updating address book…"));
    }

    void ContactsManagerWidget::deleteAddressBook(std::string accountId,
                                                  javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !book.myRights.mayDelete)
            return;
        const auto answer = QMessageBox::question(
            this, i18n("Delete Address Book"),
            i18n("Delete this address book and remove contacts that belong to no other address "
                 "book?\n\nAddress book: %1\nAccount: %2",
                 QString::fromStdString(book.name), accountLabel(m_settings, *account)));
        if (answer != QMessageBox::Yes)
            return;
        applyAddressBookMutation(
            javelin::app::DeleteAddressBookCommand{
                .accountId = std::move(accountId),
                .addressBookId = book.id,
                .removeContents = true,
            },
            i18n("Deleting address book…"));
    }

    void ContactsManagerWidget::setDefaultAddressBook(std::string accountId,
                                                      javelin::jmap::api::AddressBook book)
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end() || account->isReadOnly || !book.myRights.mayWrite)
            return;
        applyAddressBookMutation(
            javelin::app::SetDefaultAddressBookCommand{
                .accountId = std::move(accountId),
                .addressBookId = book.id,
            },
            i18n("Changing default address book…"));
    }

    bool ContactsManagerWidget::canSetAddressBookSubscription(
        const std::string_view accountId, const javelin::jmap::api::AddressBook& book,
        const bool subscribed) const
    {
        return m_addressBookController->canSetSubscription(m_accounts, accountId, book, subscribed);
    }

    void ContactsManagerWidget::setAddressBookSubscription(std::string accountId,
                                                           javelin::jmap::api::AddressBook book,
                                                           const bool subscribed)
    {
        if (!canSetAddressBookSubscription(accountId, book, subscribed))
            return;
        book.isSubscribed = subscribed;
        applyAddressBookMutation(
            javelin::app::UpdateAddressBookCommand{
                .accountId = std::move(accountId),
                .addressBook = std::move(book),
            },
            i18n("Updating subscription…"));
    }

    void ContactsManagerWidget::applyAddressBookMutation(javelin::app::AddressBookCommand command,
                                                         QString progressMessage)
    {
        m_addressBookController->mutate(std::move(command), std::move(progressMessage));
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
        return m_browser->currentAccountId();
    }

    QString ContactsManagerWidget::contactLocationLabel(
        const javelin::jmap::contacts::ContactSummary& contact) const
    {
        const auto account = std::ranges::find(m_accounts, contact.accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        QStringList books;
        const auto listedBooks = m_repository.listAddressBooks(contact.accountId);
        const auto* addressBooks =
            std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks);
        for (const auto& id : contact.addressBookIds)
        {
            if (addressBooks == nullptr)
                break;
            const auto book =
                std::ranges::find(*addressBooks, id, &javelin::jmap::api::AddressBook::id);
            if (book != addressBooks->end())
                books.push_back(QString::fromStdString(book->name));
        }
        const QString accountName = account == m_accounts.end()
                                        ? i18n("Unknown account")
                                        : accountLabel(m_settings, *account);
        return books.isEmpty() ? accountName
                               : i18nc("@info contact account and address books", "%1 — %2",
                                       accountName, books.join(QStringLiteral(", ")));
    }

    std::optional<std::string> ContactsManagerWidget::currentAddressBookId() const
    {
        return m_browser->currentAddressBookId();
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

    std::optional<std::string>
    ContactsManagerWidget::ownerAccountId(const std::string_view accountId) const
    {
        const auto account = std::ranges::find(m_accounts, accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        return account == m_accounts.end() ? std::nullopt : std::optional{account->ownerAccountId};
    }

    const javelin::jmap::contacts::ContactSummary* ContactsManagerWidget::currentContact() const
    {
        return m_browser->currentContact();
    }

    std::vector<const javelin::jmap::contacts::ContactSummary*>
    ContactsManagerWidget::selectedContacts() const
    {
        return m_browser->selectedContacts();
    }

    const javelin::jmap::contacts::ContactSummary* ContactsManagerWidget::currentGroup() const
    {
        return m_browser->currentGroup();
    }

    bool ContactsManagerWidget::groupIsWritable(
        const javelin::jmap::contacts::ContactSummary& group) const
    {
        const auto account = std::ranges::find(m_accounts, group.accountId,
                                               &javelin::jmap::cache::ContactAccount::accountId);
        if (account == m_accounts.end())
            return false;
        const auto listedBooks = m_repository.listAddressBooks(group.accountId);
        const auto* books = std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&listedBooks);
        return books != nullptr && javelin::jmap::contacts::contactActionRights(
                                       account->isReadOnly, *books, group.addressBookIds)
                                       .mayModify;
    }

    void ContactsManagerWidget::showAddressBookManager()
    {
        if (m_busy || m_accounts.empty())
            return;
        QDialog dialog{this};
        dialog.setWindowTitle(i18n("Manage Address Books"));
        dialog.resize(600, 420);
        auto* layout = new QVBoxLayout(&dialog);
        auto* account = new QComboBox(&dialog);
        for (const auto& value : m_accounts)
            account->addItem(accountLabel(m_settings, value),
                             QString::fromStdString(value.accountId));
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
        auto* create = new QPushButton(i18nc("@action:button", "New"), &dialog);
        auto* edit = new QPushButton(i18nc("@action:button", "Edit"), &dialog);
        auto* subscription = new QPushButton(i18n("Toggle Subscription"), &dialog);
        auto* remove = new QPushButton(i18nc("@action:button", "Delete"), &dialog);
        for (auto* button : {create, edit, subscription, remove})
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
                    label += i18nc("@item address book suffix", " (default)");
                if (!book.isSubscribed)
                    label += i18nc("@item address book suffix", " (unsubscribed)");
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
        const auto updateActions = [this, account, create, edit, subscription, remove, selectedBook]
        {
            const auto book = selectedBook();
            const bool selected = book.has_value();
            const auto id = account->currentData().toString().toStdString();
            const auto current =
                std::ranges::find(m_accounts, id, &javelin::jmap::cache::ContactAccount::accountId);
            const bool writableAccount = current != m_accounts.end() && !current->isReadOnly;
            create->setEnabled(writableAccount && current->mayCreateAddressBook);
            edit->setEnabled(writableAccount && selected && book->myRights.mayWrite);
            const bool canToggle =
                selected && canSetAddressBookSubscription(id, *book, !book->isSubscribed);
            subscription->setEnabled(canToggle);
            subscription->setToolTip(
                !canToggle && selected && book->isSubscribed
                    ? i18n("The only address book in an account cannot be unsubscribed.")
                    : QString{});
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
        connect(subscription, &QPushButton::clicked, &dialog,
                [this, &dialog, accountId, selectedBook]
                {
                    if (auto book = selectedBook())
                    {
                        setAddressBookSubscription(accountId(), *book, !book->isSubscribed);
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

} // namespace javelin::gui::contacts
