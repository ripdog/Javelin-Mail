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
    class ContactCommandPort;
    class ContactRefreshPort;
} // namespace javelin::app

namespace javelin::jmap::cache
{
    class ContactReader;
}

namespace javelin::gui::contacts
{
    class ContactsManagerWidget;
}
namespace javelin::gui::settings
{
    class GuiSettings;
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
        ContactsTabController(javelin::gui::settings::GuiSettings& settings,
                              javelin::jmap::cache::ContactReader& contactRepository,
                              javelin::app::ContactRefreshPort& refreshPort,
                              javelin::app::ContactCommandPort& commandPort,
                              QStackedWidget& contentStack, std::vector<TabState>& tabs,
                              QObject* parent = nullptr);

        void open();
        [[nodiscard]] bool restore(const PersistedContactsTab& persisted);
        void invoke(const TabState* tab, ContactsTabCommand command);
        void populateAddToGroupMenu(const TabState* tab, QMenu& menu) const;
        void populateRemoveFromGroupMenu(const TabState* tab, QMenu& menu) const;
        void populateAddressBookMenu(const TabState* tab, QMenu& menu) const;
        [[nodiscard]] ContactsToolbarState toolbarState(const TabState* tab) const;
        [[nodiscard]] bool available() const;
        [[nodiscard]] bool refresh(const TabState* tab);
        [[nodiscard]] bool close(TabState& tab);
        [[nodiscard]] QWidget* contentWidgetForTab(const TabState* tab) const;
        void applicationPaletteChanged();

      Q_SIGNALS:
        void tabReady(int index);
        void toolbarStateChanged();
        void statusMessage(QString message, int durationMilliseconds);
        void userInterventionRequired(QString message);
        void composeMailRequested(QString accountId, QString name, QString email);
        void searchMailFromRequested(QString accountId, QString email);

      private:
        [[nodiscard]] javelin::gui::contacts::ContactsManagerWidget* materialize();
        [[nodiscard]] javelin::gui::contacts::ContactsManagerWidget*
        widgetForTab(const TabState* tab) const;

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::jmap::cache::ContactReader& m_contactRepository;
        javelin::app::ContactRefreshPort& m_refreshPort;
        javelin::app::ContactCommandPort& m_commandPort;
        QStackedWidget& m_contentStack;
        std::vector<TabState>& m_tabs;
    };
} // namespace javelin::gui::shell
