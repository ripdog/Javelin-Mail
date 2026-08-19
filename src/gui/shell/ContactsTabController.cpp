#include "gui/shell/ContactsTabController.h"

#include "app/ContactApplicationPorts.h"
#include "gui/contacts/ContactsManagerWidget.h"
#include "gui/settings/GuiSettings.h"
#include "gui/shell/MainWindowStateStore.h"
#include "jmap/cache/ContactReader.h"

#include <KLocalizedString>

#include <QMenu>
#include <QStackedWidget>

#include <ranges>
#include <utility>
#include <variant>

namespace javelin::gui::shell
{
    ContactsTabController::ContactsTabController(
        javelin::gui::settings::GuiSettings& settings,
        javelin::jmap::cache::ContactReader& contactRepository,
        javelin::app::ContactRefreshPort& refreshPort,
        javelin::app::ContactCommandPort& commandPort, QStackedWidget& contentStack,
        std::vector<TabState>& tabs, QObject* parent)
        : QObject(parent), m_settings(settings), m_contactRepository(contactRepository),
          m_refreshPort(refreshPort), m_commandPort(commandPort), m_contentStack(contentStack),
          m_tabs(tabs)
    {
    }

    void ContactsTabController::open()
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (widgetForTab(&m_tabs[index]) != nullptr)
            {
                Q_EMIT tabReady(static_cast<int>(index));
                return;
            }
        }

        const auto result = m_contactRepository.listAccounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&result);
        if (accounts == nullptr || accounts->empty())
        {
            Q_EMIT statusMessage(i18n("The configured server does not support JMAP Contacts."),
                                 10000);
            return;
        }

        auto* widget = materialize();
        if (widget == nullptr)
            return;
        const auto index = static_cast<int>(m_tabs.size() - 1);
        Q_EMIT tabReady(index);
        widget->requestRefresh();
    }

    bool ContactsTabController::restore(const PersistedContactsTab& persisted)
    {
        auto* widget = materialize();
        if (widget == nullptr)
            return false;
        widget->restoreViewState(persisted.view);
        return true;
    }

    void ContactsTabController::invoke(const TabState* tab, const ContactsTabCommand command)
    {
        auto* widget = widgetForTab(tab);
        if (widget == nullptr)
            return;

        switch (command)
        {
        case ContactsTabCommand::CreateContact:
            widget->beginCreateContact();
            break;
        case ContactsTabCommand::CreateGroup:
            widget->beginCreateGroup();
            break;
        case ContactsTabCommand::EditContact:
            widget->beginEditContact();
            break;
        case ContactsTabCommand::DeleteContact:
            widget->deleteContact();
            break;
        case ContactsTabCommand::CopyContact:
            widget->copyContact();
            break;
        case ContactsTabCommand::ImportVCard:
            widget->importVCard();
            break;
        case ContactsTabCommand::ExportVCard:
            widget->exportVCard();
            break;
        case ContactsTabCommand::FindDuplicates:
            widget->findAndMergeDuplicates();
            break;
        case ContactsTabCommand::ManageAddressBooks:
            widget->showAddressBookManager();
            break;
        case ContactsTabCommand::Refresh:
            widget->requestRefresh();
            break;
        }
    }

    void ContactsTabController::invokeWorkspace(const ContactsTabCommand command)
    {
        const bool alreadyMaterialized = std::ranges::any_of(
            m_tabs, [this](const auto& tab) { return widgetForTab(&tab) != nullptr; });
        open();
        for (auto& tab : m_tabs)
        {
            auto* widget = widgetForTab(&tab);
            if (widget == nullptr)
                continue;

            const auto state = toolbarState(&tab);
            bool enabled = false;
            QString unavailableMessage;
            switch (command)
            {
            case ContactsTabCommand::CreateContact:
            case ContactsTabCommand::CreateGroup:
            case ContactsTabCommand::ImportVCard:
                enabled = state.canCreateContact;
                unavailableMessage = i18n("No writable address book is available.");
                break;
            case ContactsTabCommand::FindDuplicates:
                enabled = state.canFindDuplicates;
                unavailableMessage = i18n("Wait for the current Contacts operation to finish.");
                break;
            case ContactsTabCommand::ManageAddressBooks:
                enabled = state.canManageAddressBooks;
                unavailableMessage = i18n("Wait for the current Contacts operation to finish.");
                break;
            case ContactsTabCommand::Refresh:
                enabled = state.canRefresh;
                unavailableMessage = i18n("Wait for the current Contacts operation to finish.");
                break;
            case ContactsTabCommand::EditContact:
            case ContactsTabCommand::DeleteContact:
            case ContactsTabCommand::CopyContact:
            case ContactsTabCommand::ExportVCard:
                return;
            }

            if (!enabled)
            {
                Q_EMIT statusMessage(unavailableMessage, 5000);
                return;
            }
            if (command != ContactsTabCommand::Refresh || alreadyMaterialized)
                invoke(&tab, command);
            return;
        }
    }

    void ContactsTabController::populateAddToGroupMenu(const TabState* tab, QMenu& menu) const
    {
        if (auto* widget = widgetForTab(tab); widget != nullptr)
            widget->populateAddToGroupMenu(menu);
        else
            menu.clear();
    }

    void ContactsTabController::populateRemoveFromGroupMenu(const TabState* tab, QMenu& menu) const
    {
        if (auto* widget = widgetForTab(tab); widget != nullptr)
            widget->populateRemoveFromGroupMenu(menu);
        else
            menu.clear();
    }

    void ContactsTabController::populateAddressBookMenu(const TabState* tab, QMenu& menu) const
    {
        if (auto* widget = widgetForTab(tab))
            widget->populateAddressBookMenu(menu);
        else
            menu.clear();
    }

    ContactsToolbarState ContactsTabController::toolbarState(const TabState* tab) const
    {
        const auto* widget = widgetForTab(tab);
        if (widget == nullptr)
            return {};

        const bool busy = widget->operationInFlight();
        const bool selected = widget->hasSelectedContact();
        const bool singleSelected = widget->hasSingleSelectedContact();
        return {
            .available = true,
            .busy = busy,
            .canCreateContact = !busy && widget->canCreateContact(),
            .canEditContact = !busy && selected && widget->canEditContact(),
            .canDeleteContact = !busy && selected && widget->canDeleteContact(),
            .canCopyContact = !busy && singleSelected,
            .canExportContact = widget->canSaveContactAs(),
            .canFindDuplicates = !busy,
            .canAddToGroup =
                !busy && (widget->canCreateGroup() || widget->canAddSelectedContactToGroup()),
            .canRemoveFromGroup = !busy && widget->canRemoveSelectedContactFromGroup(),
            .canManageAddressBooks = !busy,
            .canRefresh = !busy,
        };
    }

    bool ContactsTabController::available() const
    {
        const auto accounts = m_contactRepository.listAccounts();
        const auto* values =
            std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&accounts);
        return values != nullptr && !values->empty();
    }

    bool ContactsTabController::refresh(const TabState* tab)
    {
        auto* widget = widgetForTab(tab);
        if (widget == nullptr)
            return false;
        widget->requestRefresh();
        return true;
    }

    bool ContactsTabController::close(TabState& tab)
    {
        auto* contactsTab = std::get_if<ContactsTabState>(&tab.content);
        if (contactsTab == nullptr || contactsTab->widget == nullptr)
            return false;
        if (contactsTab->widget->operationInFlight())
        {
            Q_EMIT statusMessage(i18n("Wait for the Contacts operation to finish."), 5000);
            return false;
        }

        auto* widget = contactsTab->widget;
        m_contentStack.removeWidget(widget);
        widget->deleteLater();
        contactsTab->widget = nullptr;
        return true;
    }

    QWidget* ContactsTabController::contentWidgetForTab(const TabState* tab) const
    {
        return widgetForTab(tab);
    }

    void ContactsTabController::applicationPaletteChanged()
    {
        for (auto& tab : m_tabs)
        {
            if (auto* widget = widgetForTab(&tab); widget != nullptr)
            {
                widget->applicationPaletteChanged();
            }
        }
    }

    javelin::gui::contacts::ContactsManagerWidget* ContactsTabController::materialize()
    {
        const auto availableAccounts = m_contactRepository.listAccounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&availableAccounts);
        if (accounts == nullptr || accounts->empty())
            return nullptr;

        auto* widget = new javelin::gui::contacts::ContactsManagerWidget(
            m_settings, m_contactRepository, m_refreshPort, m_commandPort, &m_contentStack);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::statusMessageRequested,
                this, &ContactsTabController::statusMessage);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::userInterventionRequired,
                this, &ContactsTabController::userInterventionRequired);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::composeMailRequested, this,
                &ContactsTabController::composeMailRequested);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::searchMailFromRequested,
                this, &ContactsTabController::searchMailFromRequested);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::toolbarStateChanged, this,
                [this] { Q_EMIT toolbarStateChanged(); });
        m_contentStack.addWidget(widget);
        m_tabs.push_back(TabState{.content = ContactsTabState{
                                      .title = i18n("Contacts"),
                                      .widget = widget,
                                      .selection = {},
                                  }});
        return widget;
    }

    javelin::gui::contacts::ContactsManagerWidget*
    ContactsTabController::widgetForTab(const TabState* tab) const
    {
        if (tab == nullptr)
            return nullptr;
        const auto* contactsTab = std::get_if<ContactsTabState>(&tab->content);
        return contactsTab != nullptr ? contactsTab->widget : nullptr;
    }
} // namespace javelin::gui::shell
