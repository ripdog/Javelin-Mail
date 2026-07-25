#pragma once

#include "gui/contacts/ContactsViewState.h"
#include "gui/shell/TabWorkspace.h"

#include <QObject>
#include <QString>

#include <optional>
#include <string>
#include <vector>

class QMenu;
class QStackedWidget;
class QWidget;

namespace javelin::app
{
    class MailApplicationService;
}

namespace javelin::jmap::cache
{
    class ContactRepository;
}

namespace javelin::gui::contacts
{
    class ContactsManagerWidget;
}

namespace javelin::gui::shell
{
    struct PersistedContactsTab;

    enum class ContactsTabCommand
    {
        CreateContact,
        CreateGroup,
        EditContact,
        DeleteContact,
        CopyContact,
        ImportVCard,
        ExportVCard,
        FindDuplicates,
        ManageAddressBooks,
        Refresh,
    };

    struct ContactsToolbarState
    {
        bool available = false;
        bool busy = true;
        bool canCreateContact = false;
        bool canEditContact = false;
        bool canDeleteContact = false;
        bool canCopyContact = false;
        bool canExportContact = false;
        bool canFindDuplicates = false;
        bool canAddToGroup = false;
        bool canRemoveFromGroup = false;
        bool canManageAddressBooks = false;
        bool canRefresh = false;
    };

    class ContactsTabController final : public QObject
    {
        Q_OBJECT

      public:
        ContactsTabController(javelin::jmap::cache::ContactRepository& contactRepository,
                              javelin::app::MailApplicationService& mailService,
                              QStackedWidget& contentStack, std::vector<TabState>& tabs,
                              QObject* parent = nullptr);

        void open(std::optional<std::string> preferredAccountId);
        [[nodiscard]] bool restore(const PersistedContactsTab& persisted);
        void invoke(const TabState* tab, ContactsTabCommand command);
        void populateAddToGroupMenu(const TabState* tab, QMenu& menu) const;
        void populateRemoveFromGroupMenu(const TabState* tab, QMenu& menu) const;
        [[nodiscard]] ContactsToolbarState toolbarState(const TabState* tab) const;
        [[nodiscard]] bool refresh(const TabState* tab);
        [[nodiscard]] bool close(TabState& tab);
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;

      Q_SIGNALS:
        void tabReady(int index);
        void toolbarStateChanged();
        void statusMessage(QString message, int durationMilliseconds);
        void userInterventionRequired(QString message);
        void composeMailRequested(QString accountId, QString name, QString email);
        void searchMailFromRequested(QString accountId, QString email);

      private:
        [[nodiscard]] javelin::gui::contacts::ContactsManagerWidget*
        materialize(std::string ownerAccountId, QString title);
        [[nodiscard]] javelin::gui::contacts::ContactsManagerWidget*
        widgetForTab(const TabState* tab) const;

        javelin::jmap::cache::ContactRepository& m_contactRepository;
        javelin::app::MailApplicationService& m_mailService;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
    };
} // namespace javelin::gui::shell
