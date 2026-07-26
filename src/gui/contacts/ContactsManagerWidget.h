#pragma once

#include "app/ContactApplicationPorts.h"
#include "gui/contacts/ContactsViewState.h"
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
class QMenu;
class QPoint;
class QPlainTextEdit;
class QVBoxLayout;
class QToolButton;
class QPushButton;
class QStackedWidget;

namespace javelin::gui::contacts
{
    class ContactFieldEditor;

    class ContactsManagerWidget final : public QWidget
    {
        Q_OBJECT

      public:
        ContactsManagerWidget(javelin::jmap::cache::ContactRepository& repository,
                              javelin::app::ContactRefreshPort& refreshPort,
                              javelin::app::ContactCommandPort& commandPort,
                              std::string ownerAccountId, QWidget* parent = nullptr);

        [[nodiscard]] bool operationInFlight() const;
        [[nodiscard]] bool hasSelectedContact() const;
        [[nodiscard]] bool hasSingleSelectedContact() const;
        [[nodiscard]] bool canCreateContact() const;
        [[nodiscard]] bool canEditContact() const;
        [[nodiscard]] bool canDeleteContact() const;
        [[nodiscard]] bool canCreateGroup() const;
        [[nodiscard]] bool canStarSelectedContacts() const;
        [[nodiscard]] bool canAddSelectedContactToGroup() const;
        [[nodiscard]] bool canRemoveSelectedContactFromGroup() const;
        [[nodiscard]] ContactsViewState viewState() const;
        void restoreViewState(const ContactsViewState& state);
        void populateAddToGroupMenu(QMenu& menu);
        void populateRemoveFromGroupMenu(QMenu& menu);

      public Q_SLOTS:
        void requestRefresh();
        void beginCreateContact();
        void beginCreateGroup();
        void beginEditContact();
        void deleteContact();
        void copyContact();
        void importVCard();
        void exportVCard();
        void findAndMergeDuplicates();
        void toggleContactStarred();
        void showAddressBookManager();

      Q_SIGNALS:
        void statusMessageRequested(const QString& message, int timeoutMs = 5000);
        void userInterventionRequired(const QString& message);
        void composeMailRequested(const QString& accountId, const QString& name,
                                  const QString& email);
        void searchMailFromRequested(const QString& accountId, const QString& email);
        void toolbarStateChanged(bool busy, bool hasSelectedContact);

      private:
        void setupUi();
        void reloadAccounts();
        void reloadAddressBooks();
        void reloadContacts();
        void showSelectedContact();
        void rebuildMultipleSelectionSummary(
            const std::vector<const javelin::jmap::contacts::ContactSummary*>& contacts);
        void showContactContextMenu(const QPoint& position);
        void setContactGroupMembership(std::string groupId, std::vector<std::string> memberUids,
                                       bool included);
        void cancelEdit();
        void loadEditorDocument(const QString& document);
        void saveContact();
        void uploadPhoto();
        void removePhoto();
        void showContactPhoto(const javelin::jmap::contacts::ContactSummary& contact);
        void createAddressBook(std::string accountId);
        void editAddressBook(std::string accountId, javelin::jmap::api::AddressBook book);
        void deleteAddressBook(std::string accountId, javelin::jmap::api::AddressBook book);
        void setDefaultAddressBook(std::string accountId, javelin::jmap::api::AddressBook book);
        void toggleAddressBookSubscription(std::string accountId,
                                           javelin::jmap::api::AddressBook book);
        void editAddressBookSharing(std::string accountId, javelin::jmap::api::AddressBook book);
        void applyAddressBookMutation(javelin::app::AddressBookCommand command,
                                      QString progressMessage);
        void setBusy(bool busy);
        [[nodiscard]] std::optional<std::string> currentAccountId() const;
        [[nodiscard]] std::optional<std::string> currentAddressBookId() const;
        [[nodiscard]] const javelin::jmap::cache::ContactAccount* currentAccount() const;
        [[nodiscard]] const javelin::jmap::contacts::ContactSummary* currentContact() const;
        [[nodiscard]] std::vector<const javelin::jmap::contacts::ContactSummary*>
        selectedContacts() const;
        [[nodiscard]] const javelin::jmap::contacts::ContactSummary* currentGroup() const;
        [[nodiscard]] bool
        groupIsWritable(const javelin::jmap::contacts::ContactSummary& group) const;
        void populateContactCards(const javelin::jmap::contacts::ContactSummary& contact);

        javelin::jmap::cache::ContactRepository& m_repository;
        javelin::app::ContactRefreshPort& m_refreshPort;
        javelin::app::ContactCommandPort& m_commandPort;
        std::string m_ownerAccountId;
        std::vector<javelin::jmap::cache::ContactAccount> m_accounts;
        std::vector<javelin::jmap::api::AddressBook> m_addressBooks;
        std::vector<javelin::jmap::contacts::ContactSummary> m_contacts;
        std::vector<javelin::jmap::contacts::ContactSummary> m_groups;
        bool m_busy = false;
        bool m_creating = false;
        QComboBox* m_accountCombo = nullptr;
        QComboBox* m_addressBookCombo = nullptr;
        QComboBox* m_sortCombo = nullptr;
        QLineEdit* m_filterEdit = nullptr;
        QListWidget* m_groupList = nullptr;
        QListWidget* m_contactList = nullptr;
        QStackedWidget* m_detailStack = nullptr;
        QLabel* m_multipleSelectionTitle = nullptr;
        QVBoxLayout* m_multipleSelectionLayout = nullptr;
        QToolButton* m_multipleStarButton = nullptr;
        QLabel* m_viewTitle = nullptr;
        QLabel* m_photoLabel = nullptr;
        QLabel* m_editorPhotoLabel = nullptr;
        QToolButton* m_starButton = nullptr;
        QWidget* m_cardContainer = nullptr;
        QVBoxLayout* m_cardLayout = nullptr;
        QPlainTextEdit* m_documentEdit = nullptr;
        QComboBox* m_kindEdit = nullptr;
        QLineEdit* m_nameEdit = nullptr;
        QLineEdit* m_organizationEdit = nullptr;
        QLineEdit* m_titleEdit = nullptr;
        ContactFieldEditor* m_emailsEdit = nullptr;
        ContactFieldEditor* m_phonesEdit = nullptr;
        ContactFieldEditor* m_addressesEdit = nullptr;
        QListWidget* m_membersEdit = nullptr;
        QLineEdit* m_birthdayEdit = nullptr;
        QPlainTextEdit* m_notesEdit = nullptr;
        QListWidget* m_addressBooksEdit = nullptr;
        QToolButton* m_advancedToggle = nullptr;
        QPushButton* m_saveButton = nullptr;
        QPushButton* m_uploadPhotoButton = nullptr;
        QPushButton* m_removePhotoButton = nullptr;
        QPushButton* m_cancelButton = nullptr;
    };
} // namespace javelin::gui::contacts
