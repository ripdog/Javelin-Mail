#include "gui/contacts/ContactsManagerWidget.h"

#include "jmap/contacts/ContactService.h"

#include <QCoroTask>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
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

        [[nodiscard]] QString accountLabel(const javelin::jmap::cache::ContactAccount& account)
        {
            return account.name.empty() ? QString::fromStdString(account.accountId)
                                        : QString::fromStdString(account.name);
        }
    } // namespace

    ContactsManagerWidget::ContactsManagerWidget(
        javelin::jmap::cache::ContactRepository& repository,
        javelin::jmap::contacts::ContactService& service,
        javelin::jmap::LiveConnectionSettings settings, std::string ownerAccountId, QWidget* parent)
        : QWidget(parent), m_repository(repository), m_service(service),
          m_settings(std::move(settings)), m_ownerAccountId(std::move(ownerAccountId))
    {
        setupUi();
        reloadAccounts();
    }

    bool ContactsManagerWidget::operationInFlight() const
    {
        return m_busy;
    }

    void ContactsManagerWidget::setupUi()
    {
        auto* root = new QVBoxLayout(this);
        auto* top = new QHBoxLayout();
        m_accountCombo = new QComboBox(this);
        m_addressBookCombo = new QComboBox(this);
        m_addBookButton = new QPushButton(QStringLiteral("New Address Book"), this);
        m_editBookButton = new QPushButton(QStringLiteral("Edit"), this);
        m_deleteBookButton = new QPushButton(QStringLiteral("Delete"), this);
        m_defaultBookButton = new QPushButton(QStringLiteral("Make Default"), this);
        m_subscribeBookButton = new QPushButton(QStringLiteral("Subscribe"), this);
        m_refreshButton = new QPushButton(QStringLiteral("Refresh"), this);
        top->addWidget(new QLabel(QStringLiteral("Account"), this));
        top->addWidget(m_accountCombo);
        top->addWidget(new QLabel(QStringLiteral("Address book"), this));
        top->addWidget(m_addressBookCombo, 1);
        top->addWidget(m_addBookButton);
        top->addWidget(m_editBookButton);
        top->addWidget(m_deleteBookButton);
        top->addWidget(m_defaultBookButton);
        top->addWidget(m_subscribeBookButton);
        top->addWidget(m_refreshButton);
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
        leftLayout->addWidget(m_contactList, 1);
        auto* contactButtons = new QHBoxLayout();
        m_newContactButton = new QPushButton(QStringLiteral("New"), left);
        m_editContactButton = new QPushButton(QStringLiteral("Edit"), left);
        m_deleteContactButton = new QPushButton(QStringLiteral("Delete"), left);
        contactButtons->addWidget(m_newContactButton);
        contactButtons->addWidget(m_editContactButton);
        contactButtons->addWidget(m_deleteContactButton);
        leftLayout->addLayout(contactButtons);

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
        m_viewDetails = new QLabel(view);
        m_viewDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_viewDetails->setWordWrap(true);
        auto* fullDocument = new QPlainTextEdit(view);
        fullDocument->setReadOnly(true);
        fullDocument->setObjectName(QStringLiteral("contactDocumentView"));
        viewLayout->addWidget(m_viewTitle);
        viewLayout->addWidget(m_viewDetails);
        viewLayout->addWidget(new QLabel(QStringLiteral("Complete JSContact document"), view));
        viewLayout->addWidget(fullDocument, 1);
        m_detailStack->addWidget(view);
        auto* edit = new QWidget(m_detailStack);
        auto* editLayout = new QVBoxLayout(edit);
        auto* hint = new QLabel(
            QStringLiteral("Edit the complete JSContact Card. Unknown and extension properties are "
                           "preserved. The immutable server id is removed automatically on save."),
            edit);
        hint->setWordWrap(true);
        m_documentEdit = new QPlainTextEdit(edit);
        auto* editButtons = new QHBoxLayout();
        m_uploadPhotoButton = new QPushButton(QStringLiteral("Upload Photo"), edit);
        editButtons->addWidget(m_uploadPhotoButton);
        editButtons->addStretch(1);
        m_cancelButton = new QPushButton(QStringLiteral("Cancel"), edit);
        m_saveButton = new QPushButton(QStringLiteral("Save"), edit);
        editButtons->addWidget(m_cancelButton);
        editButtons->addWidget(m_saveButton);
        editLayout->addWidget(hint);
        editLayout->addWidget(m_documentEdit, 1);
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
        connect(m_newContactButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::beginCreateContact);
        connect(m_editContactButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::beginEditContact);
        connect(m_deleteContactButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::deleteContact);
        connect(m_saveButton, &QPushButton::clicked, this, &ContactsManagerWidget::saveContact);
        connect(m_uploadPhotoButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::uploadPhoto);
        connect(m_cancelButton, &QPushButton::clicked, this, &ContactsManagerWidget::cancelEdit);
        connect(m_refreshButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::refreshRemote);
        connect(m_addBookButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::createAddressBook);
        connect(m_editBookButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::editAddressBook);
        connect(m_deleteBookButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::deleteAddressBook);
        connect(m_defaultBookButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::setDefaultAddressBook);
        connect(m_subscribeBookButton, &QPushButton::clicked, this,
                &ContactsManagerWidget::toggleAddressBookSubscription);
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
        for (const auto& contact : m_contacts)
        {
            auto* item =
                new QListWidgetItem(QString::fromStdString(contact.displayName), m_contactList);
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
            m_editContactButton->setEnabled(false);
            m_deleteContactButton->setEnabled(false);
            return;
        }
        m_viewTitle->setText(QString::fromStdString(contact->displayName));
        m_viewDetails->setText(contactDetails(*contact));
        if (auto* document = m_detailStack->widget(1)->findChild<QPlainTextEdit*>(
                QStringLiteral("contactDocumentView")))
            document->setPlainText(QString::fromStdString(contact->document));
        m_detailStack->setCurrentIndex(1);
        m_editContactButton->setEnabled(!m_busy);
        m_deleteContactButton->setEnabled(!m_busy);
    }

    void ContactsManagerWidget::beginCreateContact()
    {
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
        m_documentEdit->setPlainText(document);
        m_detailStack->setCurrentIndex(2);
    }

    void ContactsManagerWidget::beginEditContact()
    {
        const auto* contact = currentContact();
        if (contact == nullptr)
            return;
        m_creating = false;
        m_documentEdit->setPlainText(QString::fromStdString(contact->document));
        m_detailStack->setCurrentIndex(2);
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
        const auto prepared = javelin::jmap::contacts::prepareContactDocument(
            m_documentEdit->toPlainText().toStdString(), m_creating);
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
        auto task = m_service.setContactCards(m_settings, m_ownerAccountId, std::move(request));
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
                           refreshRemote();
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
        auto task = m_service.uploadMedia(m_settings, m_ownerAccountId, *accountId, payload,
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
        auto task = m_service.setContactCards(m_settings, m_ownerAccountId, std::move(request));
        QCoro::connect(std::move(task), this,
                       [this](javelin::jmap::contacts::ContactMutationResult result)
                       {
                           setBusy(false);
                           if (const auto* error =
                                   std::get_if<javelin::jmap::LiveRefreshError>(&result))
                               Q_EMIT statusMessageRequested(error->message, 10000);
                           else
                               refreshRemote();
                       });
    }

    void ContactsManagerWidget::refreshRemote()
    {
        if (m_busy)
            return;
        setBusy(true);
        Q_EMIT statusMessageRequested(QStringLiteral("Refreshing contacts…"), 5000);
        auto task = m_service.refreshAll(m_settings, m_ownerAccountId);
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

    void ContactsManagerWidget::createAddressBook()
    {
        const auto accountId = currentAccountId();
        if (!accountId.has_value())
            return;
        javelin::jmap::api::AddressBook book;
        book.isSubscribed = true;
        AddressBookDialog dialog{book, this};
        if (dialog.exec() != QDialog::Accepted)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = *accountId;
        request.create.emplace("new-address-book",
                               javelin::jmap::api::addressBookCreateDocument(dialog.value()));
        applyAddressBookSet(std::move(request), QStringLiteral("Creating address book…"));
    }

    void ContactsManagerWidget::editAddressBook()
    {
        const auto accountId = currentAccountId();
        auto* book = currentAddressBook();
        if (!accountId.has_value() || book == nullptr)
            return;
        AddressBookDialog dialog{*book, this};
        if (dialog.exec() != QDialog::Accepted)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = *accountId;
        request.update.emplace(book->id,
                               javelin::jmap::api::addressBookUpdateDocument(dialog.value()));
        applyAddressBookSet(std::move(request), QStringLiteral("Updating address book…"));
    }

    void ContactsManagerWidget::deleteAddressBook()
    {
        const auto accountId = currentAccountId();
        auto* book = currentAddressBook();
        if (!accountId.has_value() || book == nullptr)
            return;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Delete Address Book"),
            QStringLiteral(
                "Delete %1 and remove its contacts that belong to no other address book?")
                .arg(QString::fromStdString(book->name)));
        if (answer != QMessageBox::Yes)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = *accountId;
        request.destroy.push_back(book->id);
        request.onDestroyRemoveContents = true;
        applyAddressBookSet(std::move(request), QStringLiteral("Deleting address book…"));
    }

    void ContactsManagerWidget::setDefaultAddressBook()
    {
        const auto accountId = currentAccountId();
        auto* book = currentAddressBook();
        if (!accountId.has_value() || book == nullptr)
            return;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = *accountId;
        request.onSuccessSetIsDefault = book->id;
        applyAddressBookSet(std::move(request), QStringLiteral("Changing default address book…"));
    }

    void ContactsManagerWidget::toggleAddressBookSubscription()
    {
        const auto accountId = currentAccountId();
        auto* book = currentAddressBook();
        if (!accountId.has_value() || book == nullptr)
            return;
        auto changed = *book;
        changed.isSubscribed = !changed.isSubscribed;
        javelin::jmap::api::AddressBookSetRequest request;
        request.accountId = *accountId;
        request.update.emplace(book->id, javelin::jmap::api::addressBookUpdateDocument(changed));
        applyAddressBookSet(std::move(request), QStringLiteral("Updating subscription…"));
    }

    void
    ContactsManagerWidget::applyAddressBookSet(javelin::jmap::api::AddressBookSetRequest request,
                                               QString progressMessage)
    {
        setBusy(true);
        Q_EMIT statusMessageRequested(progressMessage, 5000);
        auto task = m_service.setAddressBooks(m_settings, m_ownerAccountId, std::move(request));
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
                           refreshRemote();
                       });
    }

    void ContactsManagerWidget::setBusy(const bool busy)
    {
        m_busy = busy;
        for (auto* button :
             {m_newContactButton, m_editContactButton, m_deleteContactButton, m_refreshButton,
              m_saveButton, m_addBookButton, m_editBookButton, m_deleteBookButton,
              m_defaultBookButton, m_subscribeBookButton, m_uploadPhotoButton})
            button->setEnabled(!busy);
        m_accountCombo->setEnabled(!busy);
        m_addressBookCombo->setEnabled(!busy);
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

    javelin::jmap::api::AddressBook* ContactsManagerWidget::currentAddressBook()
    {
        const auto id = currentAddressBookId();
        if (!id.has_value())
            return nullptr;
        const auto found =
            std::ranges::find(m_addressBooks, *id, &javelin::jmap::api::AddressBook::id);
        return found == m_addressBooks.end() ? nullptr : &*found;
    }

    QString ContactsManagerWidget::contactDetails(
        const javelin::jmap::contacts::ContactSummary& contact) const
    {
        QStringList lines;
        lines << QStringLiteral("Type: %1").arg(QString::fromStdString(contact.kind));
        if (contact.organization.has_value())
            lines << QStringLiteral("Organization: %1")
                         .arg(QString::fromStdString(*contact.organization));
        for (const auto& email : contact.emails)
            lines << QStringLiteral("Email: %1").arg(QString::fromStdString(email.address));
        lines << QStringLiteral("UID: %1").arg(QString::fromStdString(contact.uid));
        return lines.join(QLatin1Char('\n'));
    }
} // namespace javelin::gui::contacts
