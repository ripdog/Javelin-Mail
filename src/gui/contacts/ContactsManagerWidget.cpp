#include "gui/contacts/ContactsManagerWidget.h"

#include "gui/IconUtils.h"
#include "jmap/contacts/ContactService.h"

#include <QCoroTask>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace javelin::gui::contacts
{
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
            return account.name.empty() ? QString::fromStdString(account.accountId)
                                        : QString::fromStdString(account.name);
        }

        [[nodiscard]] QString lines(const std::vector<std::string>& values)
        {
            QStringList result;
            for (const auto& value : values)
                result.push_back(QString::fromStdString(value));
            return result.join(QLatin1Char('\n'));
        }

        [[nodiscard]] std::vector<std::string> nonEmptyLines(const QString& value)
        {
            std::vector<std::string> result;
            for (const auto& line : value.split(QLatin1Char('\n')))
            {
                const auto trimmed = line.trimmed();
                if (!trimmed.isEmpty())
                    result.push_back(trimmed.toStdString());
            }
            return result;
        }
    } // namespace

    ContactsManagerWidget::ContactsManagerWidget(
        javelin::jmap::cache::ContactRepository& repository,
        javelin::app::MailApplicationService& service, std::string ownerAccountId, QWidget* parent)
        : QWidget(parent), m_repository(repository), m_service(service),
          m_ownerAccountId(std::move(ownerAccountId))
    {
        setupUi();
        reloadAccounts();
    }

    bool ContactsManagerWidget::operationInFlight() const
    {
        return m_busy;
    }

    bool ContactsManagerWidget::hasSelectedContact() const
    {
        return currentContact() != nullptr;
    }

    void ContactsManagerWidget::setupUi()
    {
        setObjectName(QStringLiteral("contactsManager"));
        setStyleSheet(QStringLiteral(
            "#contactsManager QLineEdit, #contactsManager QPlainTextEdit, #contactsManager "
            "QComboBox, #contactsManager QListWidget { border: 1px solid palette(mid); "
            "border-radius: 7px; padding: 6px; background: palette(base); }"
            "#contactsManager QPushButton { padding: 7px 11px; border-radius: 7px; }"
            "#contactsManager QToolButton { padding: 7px; font-weight: 600; }"
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
        m_contactList->setContextMenuPolicy(Qt::CustomContextMenu);
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
        viewLayout->addWidget(m_viewTitle);
        viewLayout->addWidget(cardScroll, 1);
        viewLayout->addWidget(viewAdvanced);
        viewLayout->addWidget(fullDocument, 1);
        m_detailStack->addWidget(view);
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

        auto* identityForm = new QFormLayout();
        identityForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
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
        identityForm->addRow(QStringLiteral("Type"), m_kindEdit);
        identityForm->addRow(QStringLiteral("Name"), m_nameEdit);
        identityForm->addRow(QStringLiteral("Organization"), m_organizationEdit);
        identityForm->addRow(QStringLiteral("Title"), m_titleEdit);
        formLayout->addLayout(identityForm);

        auto* contactForm = new QFormLayout();
        m_emailsEdit = new QPlainTextEdit(formWidget);
        m_emailsEdit->setPlaceholderText(QStringLiteral("One email address per line"));
        m_emailsEdit->setMaximumHeight(86);
        m_phonesEdit = new QPlainTextEdit(formWidget);
        m_phonesEdit->setPlaceholderText(QStringLiteral("One phone number per line"));
        m_phonesEdit->setMaximumHeight(86);
        m_addressesEdit = new QPlainTextEdit(formWidget);
        m_addressesEdit->setPlaceholderText(QStringLiteral("One postal address per line"));
        m_addressesEdit->setMaximumHeight(100);
        contactForm->addRow(QStringLiteral("Emails"), m_emailsEdit);
        contactForm->addRow(QStringLiteral("Phones"), m_phonesEdit);
        contactForm->addRow(QStringLiteral("Addresses"), m_addressesEdit);
        formLayout->addLayout(contactForm);

        auto* personalForm = new QFormLayout();
        m_birthdayEdit = new QLineEdit(formWidget);
        m_birthdayEdit->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
        m_notesEdit = new QPlainTextEdit(formWidget);
        m_notesEdit->setPlaceholderText(QStringLiteral("Notes"));
        m_notesEdit->setMaximumHeight(100);
        m_addressBooksEdit = new QListWidget(formWidget);
        m_addressBooksEdit->setMaximumHeight(110);
        personalForm->addRow(QStringLiteral("Birthday"), m_birthdayEdit);
        personalForm->addRow(QStringLiteral("Notes"), m_notesEdit);
        personalForm->addRow(QStringLiteral("Address books"), m_addressBooksEdit);
        formLayout->addLayout(personalForm);

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
        m_uploadPhotoButton = new QPushButton(QStringLiteral("Upload Photo"), edit);
        editButtons->addWidget(m_uploadPhotoButton);
        editButtons->addStretch(1);
        m_cancelButton = new QPushButton(QStringLiteral("Cancel"), edit);
        m_saveButton = new QPushButton(QStringLiteral("Save"), edit);
        editButtons->addWidget(m_cancelButton);
        editButtons->addWidget(m_saveButton);
        editLayout->addWidget(scroll, 1);
        editLayout->addLayout(editButtons);
        m_detailStack->addWidget(edit);
        splitter->addWidget(left);
        splitter->addWidget(m_detailStack);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 2);
        root->addWidget(splitter, 1);

        connect(m_accountCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { reloadAddressBooks(); });
        connect(m_addressBookCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { reloadContacts(); });
        connect(m_filterEdit, &QLineEdit::textChanged, this, [this] { reloadContacts(); });
        connect(m_sortCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { reloadContacts(); });
        connect(m_contactList, &QListWidget::currentRowChanged, this,
                [this] { showSelectedContact(); });
        connect(m_contactList, &QListWidget::customContextMenuRequested, this,
                &ContactsManagerWidget::showContactContextMenu);
        connect(m_saveButton, &QPushButton::clicked, this, &ContactsManagerWidget::saveContact);
        connect(m_uploadPhotoButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::uploadPhoto);
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
        m_contacts.clear();
        m_contactList->clear();
        if (!accountId.has_value())
        {
            showSelectedContact();
            return;
        }
        const auto bookId = currentAddressBookId();
        const auto result = m_repository.listContacts(
            *accountId,
            bookId.has_value() ? std::optional<std::string_view>{*bookId} : std::nullopt,
            m_filterEdit->text().toStdString());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
        {
            Q_EMIT statusMessageRequested(error->message, 10000);
            return;
        }
        m_contacts = std::get<std::vector<javelin::jmap::contacts::ContactSummary>>(result);
        const int sort = m_sortCombo->currentData().toInt();
        std::ranges::sort(m_contacts,
                          [sort](const auto& left, const auto& right)
                          {
                              if (left.isImportant != right.isImportant)
                                  return left.isImportant;
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
        bool addedRegularSeparator = false;
        for (const auto& contact : m_contacts)
        {
            if (!contact.isImportant && !addedRegularSeparator &&
                std::ranges::any_of(m_contacts,
                                    &javelin::jmap::contacts::ContactSummary::isImportant))
            {
                auto* separator =
                    new QListWidgetItem(QStringLiteral("ALL CONTACTS"), m_contactList);
                separator->setFlags(Qt::NoItemFlags);
                QFont font = separator->font();
                font.setBold(true);
                separator->setFont(font);
                addedRegularSeparator = true;
            }
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
        }
        if (!m_contacts.empty())
            m_contactList->setCurrentRow(0);
        else
            showSelectedContact();
    }

    void ContactsManagerWidget::showSelectedContact()
    {
        const auto* contact = currentContact();
        if (contact == nullptr)
        {
            m_detailStack->setCurrentIndex(0);
            Q_EMIT toolbarStateChanged(m_busy, false);
            return;
        }
        m_viewTitle->setText(QString::fromStdString(contact->displayName));
        populateContactCards(*contact);
        if (auto* document = m_detailStack->widget(1)->findChild<QPlainTextEdit*>(
                QStringLiteral("contactDocumentView")))
            document->setPlainText(QString::fromStdString(contact->document));
        m_detailStack->setCurrentIndex(1);
        Q_EMIT toolbarStateChanged(m_busy, true);
    }

    void ContactsManagerWidget::showContactContextMenu(const QPoint& position)
    {
        auto* item = m_contactList->itemAt(position);
        if (item == nullptr || !(item->flags() & Qt::ItemIsSelectable))
            return;
        m_contactList->setCurrentItem(item);
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
        menu.addSeparator();
        auto* edit = menu.addAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                    QStringLiteral("Edit Contact"));
        connect(edit, &QAction::triggered, this, &ContactsManagerWidget::beginEditContact);
        auto* copy = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                    QStringLiteral("Copy Contact…"));
        connect(copy, &QAction::triggered, this, &ContactsManagerWidget::copyContact);
        auto* remove = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                      QStringLiteral("Delete Contact"));
        connect(remove, &QAction::triggered, this, &ContactsManagerWidget::deleteContact);
        for (auto* action : {edit, copy, remove})
            action->setEnabled(!m_busy);
        menu.exec(m_contactList->viewport()->mapToGlobal(position));
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
        m_emailsEdit->setPlainText(lines(editorData->emails));
        m_phonesEdit->setPlainText(lines(editorData->phones));
        m_addressesEdit->setPlainText(lines(editorData->addresses));
        m_birthdayEdit->setText(QString::fromStdString(editorData->birthday));
        m_notesEdit->setPlainText(QString::fromStdString(editorData->notes));
        m_documentEdit->setPlainText(document);
        m_addressBooksEdit->clear();
        for (const auto& book : m_addressBooks)
        {
            auto* item = new QListWidgetItem(QString::fromStdString(book.name), m_addressBooksEdit);
            item->setData(Qt::UserRole, QString::fromStdString(book.id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(std::ranges::find(editorData->addressBookIds, book.id) !=
                                        editorData->addressBookIds.end()
                                    ? Qt::Checked
                                    : Qt::Unchecked);
        }
        m_advancedToggle->setChecked(false);
        m_detailStack->setCurrentIndex(2);
    }

    void ContactsManagerWidget::beginCreateContact()
    {
        if (m_busy)
            return;
        auto bookId = currentAddressBookId();
        if (!bookId.has_value())
        {
            const auto defaultBook = std::ranges::find_if(m_addressBooks, [](const auto& book)
                                                          { return book.isDefault; });
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
        if (m_busy)
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
        editor.emails = nonEmptyLines(m_emailsEdit->toPlainText());
        editor.phones = nonEmptyLines(m_phonesEdit->toPlainText());
        editor.addresses = nonEmptyLines(m_addressesEdit->toPlainText());
        editor.birthday = m_birthdayEdit->text().trimmed().toStdString();
        editor.notes = m_notesEdit->toPlainText().trimmed().toStdString();
        editor.document = m_documentEdit->toPlainText().toStdString();
        for (int row = 0; row < m_addressBooksEdit->count(); ++row)
        {
            const auto* item = m_addressBooksEdit->item(row);
            if (item->checkState() == Qt::Checked)
                editor.addressBookIds.push_back(item->data(Qt::UserRole).toString().toStdString());
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
                                   std::get_if<javelin::jmap::LiveRefreshError>(&result))
                           {
                               Q_EMIT statusMessageRequested(error->message, 10000);
                               if (error->requiresUserIntervention)
                                   Q_EMIT userInterventionRequired(error->message);
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
            [this](javelin::jmap::contacts::ContactUploadResult result)
            {
                setBusy(false);
                if (const auto* error = std::get_if<javelin::jmap::LiveRefreshError>(&result))
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
                Q_EMIT statusMessageRequested(
                    QStringLiteral("Photo uploaded; save the contact to apply it."), 5000);
            });
    }

    void ContactsManagerWidget::deleteContact()
    {
        if (m_busy)
            return;
        const auto accountId = currentAccountId();
        const auto* contact = currentContact();
        if (!accountId.has_value() || contact == nullptr ||
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
                                   std::get_if<javelin::jmap::LiveRefreshError>(&result))
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

        QStringList accountLabels;
        for (const auto& account : m_accounts)
            accountLabels.push_back(accountLabel(account));
        bool accepted = false;
        const QString selectedAccount = QInputDialog::getItem(this, QStringLiteral("Copy Contact"),
                                                              QStringLiteral("Destination account"),
                                                              accountLabels, 0, false, &accepted);
        const qsizetype accountIndex = accountLabels.indexOf(selectedAccount);
        if (!accepted || accountIndex < 0)
            return;
        const auto& destinationAccount = m_accounts[static_cast<std::size_t>(accountIndex)];
        const auto booksResult = m_repository.listAddressBooks(destinationAccount.accountId);
        const auto* books = std::get_if<std::vector<javelin::jmap::api::AddressBook>>(&booksResult);
        if (books == nullptr || books->empty())
        {
            QMessageBox::information(this, QStringLiteral("Copy Contact"),
                                     QStringLiteral("The destination has no address book."));
            return;
        }
        QStringList bookLabels;
        for (const auto& book : *books)
            bookLabels.push_back(QString::fromStdString(book.name));
        const QString selectedBook = QInputDialog::getItem(
            this, QStringLiteral("Copy Contact"), QStringLiteral("Destination address book"),
            bookLabels, 0, false, &accepted);
        const qsizetype bookIndex = bookLabels.indexOf(selectedBook);
        if (!accepted || bookIndex < 0)
            return;
        const auto& book = (*books)[static_cast<std::size_t>(bookIndex)];

        javelin::jmap::api::ContactCardCopyRequest request;
        request.fromAccountId = *sourceAccountId;
        request.accountId = destinationAccount.accountId;
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
                                   std::get_if<javelin::jmap::LiveRefreshError>(&result))
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
                                   std::get_if<javelin::jmap::LiveRefreshError>(&result))
                           {
                               Q_EMIT statusMessageRequested(error->message, 10000);
                               if (error->requiresUserIntervention)
                                   Q_EMIT userInterventionRequired(error->message);
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
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = std::move(accountId);
        request.onSuccessSetIsDefault = book.id;
        applyAddressBookSet(std::move(request), QStringLiteral("Changing default address book…"));
    }

    void ContactsManagerWidget::toggleAddressBookSubscription(std::string accountId,
                                                              javelin::jmap::api::AddressBook book)
    {
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
        if (!book.myRights.mayShare)
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
                                   std::get_if<javelin::jmap::LiveRefreshError>(&result))
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
        const std::array<QWidget*, 3> buttons{m_saveButton, m_uploadPhotoButton, m_cancelButton};
        for (auto* button : buttons)
            button->setEnabled(!busy);
        m_accountCombo->setEnabled(!busy);
        m_addressBookCombo->setEnabled(!busy);
        Q_EMIT toolbarStateChanged(m_busy, currentContact() != nullptr);
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

    const javelin::jmap::contacts::ContactSummary* ContactsManagerWidget::currentContact() const
    {
        const auto* item = m_contactList->currentItem();
        if (item == nullptr)
            return nullptr;
        const auto id = item->data(Qt::UserRole).toString().toStdString();
        const auto found =
            std::ranges::find(m_contacts, id, &javelin::jmap::contacts::ContactSummary::id);
        return found == m_contacts.end() ? nullptr : &*found;
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
        const auto updateActions = [edit, makeDefault, subscription, sharing, remove, selectedBook]
        {
            const auto book = selectedBook();
            const bool selected = book.has_value();
            edit->setEnabled(selected && book->myRights.mayWrite);
            makeDefault->setEnabled(selected && !book->isDefault);
            subscription->setEnabled(selected);
            sharing->setEnabled(selected && book->myRights.mayShare);
            remove->setEnabled(selected && book->myRights.mayDelete);
        };
        connect(account, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
                [loadBooks] { loadBooks(); });
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
        for (const auto& email : editorData->emails)
            addCard(QStringLiteral("Email"), QString::fromStdString(email),
                    QString::fromStdString(email), QString::fromStdString(editorData->fullName));
        for (const auto& phone : editorData->phones)
            addCard(QStringLiteral("Phone"), QString::fromStdString(phone));
        for (const auto& address : editorData->addresses)
            addCard(QStringLiteral("Address"), QString::fromStdString(address));
        if (!editorData->birthday.empty())
            addCard(QStringLiteral("Birthday"), QString::fromStdString(editorData->birthday));
        if (!editorData->notes.empty())
            addCard(QStringLiteral("Notes"), QString::fromStdString(editorData->notes));
        m_cardLayout->addStretch(1);
    }
} // namespace javelin::gui::contacts
