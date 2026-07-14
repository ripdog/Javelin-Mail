#pragma once

#include "app/LongPollCoordinator.h"
#include "jmap/api/ContactsMethods.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/contacts/ContactTypes.h"

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QVBoxLayout;
class QToolButton;
class QPushButton;
class QStackedWidget;

namespace javelin::gui::contacts
{
    class ContactsManagerWidget final : public QWidget
    {
        Q_OBJECT

      public:
        ContactsManagerWidget(javelin::jmap::cache::ContactRepository& repository,
                              javelin::app::MailApplicationService& service,
                              std::string ownerAccountId, QWidget* parent = nullptr);

        [[nodiscard]] bool operationInFlight() const;

      public Q_SLOTS:
        void requestRefresh();
        void beginCreateContact();
        void beginEditContact();
        void deleteContact();
        void copyContact();

      Q_SIGNALS:
        void statusMessageRequested(const QString& message, int timeoutMs = 5000);
        void userInterventionRequired(const QString& message);
        void composeMailRequested(const QString& accountId, const QString& name,
                                  const QString& email);

      private:
        void setupUi();
        void reloadAccounts();
        void reloadAddressBooks();
        void reloadContacts();
        void showSelectedContact();
        void cancelEdit();
        void loadEditorDocument(const QString& document);
        void saveContact();
        void uploadPhoto();
        void createAddressBook();
        void editAddressBook();
        void deleteAddressBook();
        void setDefaultAddressBook();
        void toggleAddressBookSubscription();
        void editAddressBookSharing();
        void applyAddressBookSet(javelin::jmap::api::AddressBookSetRequest request,
                                 QString progressMessage);
        void setBusy(bool busy);
        [[nodiscard]] std::optional<std::string> currentAccountId() const;
        [[nodiscard]] std::optional<std::string> currentAddressBookId() const;
        [[nodiscard]] const javelin::jmap::contacts::ContactSummary* currentContact() const;
        [[nodiscard]] javelin::jmap::api::AddressBook* currentAddressBook();
        void showAddressBookManager();
        void populateContactCards(const javelin::jmap::contacts::ContactSummary& contact);

        javelin::jmap::cache::ContactRepository& m_repository;
        javelin::app::MailApplicationService& m_service;
        std::string m_ownerAccountId;
        std::vector<javelin::jmap::cache::ContactAccount> m_accounts;
        std::vector<javelin::jmap::api::AddressBook> m_addressBooks;
        std::vector<javelin::jmap::contacts::ContactSummary> m_contacts;
        bool m_busy = false;
        bool m_creating = false;
        QComboBox* m_accountCombo = nullptr;
        QComboBox* m_addressBookCombo = nullptr;
        QComboBox* m_sortCombo = nullptr;
        QLineEdit* m_filterEdit = nullptr;
        QListWidget* m_contactList = nullptr;
        QStackedWidget* m_detailStack = nullptr;
        QLabel* m_viewTitle = nullptr;
        QWidget* m_cardContainer = nullptr;
        QVBoxLayout* m_cardLayout = nullptr;
        QPlainTextEdit* m_documentEdit = nullptr;
        QComboBox* m_kindEdit = nullptr;
        QLineEdit* m_nameEdit = nullptr;
        QLineEdit* m_organizationEdit = nullptr;
        QLineEdit* m_titleEdit = nullptr;
        QPlainTextEdit* m_emailsEdit = nullptr;
        QPlainTextEdit* m_phonesEdit = nullptr;
        QPlainTextEdit* m_addressesEdit = nullptr;
        QLineEdit* m_birthdayEdit = nullptr;
        QPlainTextEdit* m_notesEdit = nullptr;
        QListWidget* m_addressBooksEdit = nullptr;
        QToolButton* m_advancedToggle = nullptr;
        QPushButton* m_newContactButton = nullptr;
        QPushButton* m_editContactButton = nullptr;
        QPushButton* m_deleteContactButton = nullptr;
        QPushButton* m_copyContactButton = nullptr;
        QToolButton* m_refreshButton = nullptr;
        QToolButton* m_manageBooksButton = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_uploadPhotoButton = nullptr;
        QPushButton* m_cancelButton = nullptr;
        QPushButton* m_addBookButton = nullptr;
        QPushButton* m_editBookButton = nullptr;
        QPushButton* m_deleteBookButton = nullptr;
        QPushButton* m_defaultBookButton = nullptr;
        QPushButton* m_subscribeBookButton = nullptr;
        QPushButton* m_shareBookButton = nullptr;
    };
} // namespace javelin::gui::contacts
