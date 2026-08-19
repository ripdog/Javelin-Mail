#include "gui/shell/MainWindow.h"

#include "app/AccountApplicationPorts.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/ComposeApplicationPorts.h"
#include "app/IdentityApplicationPorts.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MailboxSession.h"
#include "app/MessageContentApplicationPorts.h"
#include "app/MessageListSession.h"
#include "app/MessageListSessionFactory.h"
#include "app/MessageNavigationPort.h"
#include "app/PerformanceMetrics.h"
#include "app/SearchSession.h"
#include "app/UndoApplicationPorts.h"
#include "gui/FontUtils.h"
#include "gui/IconUtils.h"
#include "gui/calendar/CalendarEventContextMenuEditorDialog.h"
#include "gui/compose/MailtoParser.h"
#include "gui/developer/DeveloperOptionsDialog.h"
#include "gui/identity/IdentityManagerDialog.h"
#include "gui/logging/LogViewerDialog.h"
#include "gui/mailboxes/MailboxPresentation.h"
#include "gui/mailboxes/MailboxPropertiesDialog.h"
#include "gui/mailboxes/MailboxSelection.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/mailboxes/MailboxTreeView.h"
#include "gui/messages/InfiniteScroll.h"
#include "gui/messages/MessageDragListView.h"
#include "gui/messages/MessageListDelegate.h"
#include "gui/messages/MessageListModel.h"
#include "gui/messages/MessageListPanePresenter.h"
#include "gui/messages/MessageSelectionRestoration.h"
#include "gui/messageview/MessageViewContainer.h"
#include "gui/search/AdvancedSearchDialog.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/AccountRefreshController.h"
#include "gui/shell/AuthenticationPromptCoordinator.h"
#include "gui/shell/CalendarTabController.h"
#include "gui/shell/ComposeTabController.h"
#include "gui/shell/ComposeTabPolicy.h"
#include "gui/shell/ContactsTabController.h"
#include "gui/shell/ElidingLabel.h"
#include "gui/shell/EmailContextMenuEditorDialog.h"
#include "gui/shell/FocusedCommandRouter.h"
#include "gui/shell/LayeredStatusBar.h"
#include "gui/shell/MailActionController.h"
#include "gui/shell/MailExportController.h"
#include "gui/shell/MailWorkspaceController.h"
#include "gui/shell/MainWindowStateStore.h"
#include "gui/shell/MessageCommandController.h"
#include "gui/shell/MessageContentController.h"
#include "gui/shell/MessageContentPolicy.h"
#include "gui/shell/MessageFileController.h"
#include "gui/shell/MessageListTabBindingPresenter.h"
#include "gui/shell/MessageListTabController.h"
#include "gui/shell/MessageListTabPresenter.h"
#include "gui/shell/MessageNavigationController.h"
#include "gui/shell/MessageSelectionController.h"
#include "gui/shell/QuickFilterController.h"
#include "gui/shell/TabActivationPolicy.h"
#include "gui/shell/TabBarPresenter.h"
#include "gui/shell/TabPersistence.h"
#include "gui/shell/ThemeController.h"
#include "gui/sieve/SieveEditorDialog.h"
#include "gui/translation/TranslationService.h"
#include "gui/widgets/IndeterminateProgressBar.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/cache/MailTagReader.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MessageViewReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactResults.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KLocalizedString>
#include <KStandardAction>
#include <KToolBar>
#include <KXMLGUIFactory>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLoggingCategory>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <QWidget>

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>
#include <variant>

namespace javelin::gui::shell
{
    Q_LOGGING_CATEGORY(logGuiMailbox, "gui.mailbox")
    Q_LOGGING_CATEGORY(logUserOperations, "user.operations")
    void MainWindow::presentError(const javelin::jmap::OperationError& error)
    {
        qCWarning(logUserOperations).noquote() << "operation failed" << error.message;
        m_statusBar->showMessage(error.message, 10000);
    }

    void MainWindow::presentUserInterventionError(const QString& message)
    {
        m_statusBar->showMessage(message, 10000);
        QMessageBox::critical(this, i18n("Action Required"), message);
    }

    namespace
    {
        /// Returns the account ID for the currently selected mailbox tree index.
        /// Works for both account-level nodes and mailbox nodes.
        [[nodiscard]] std::optional<std::string> currentAccountId(const QTreeView& mailboxView)
        {
            const auto current = mailboxView.currentIndex();
            if (!current.isValid())
            {
                return std::nullopt;
            }

            const auto accountId =
                current.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString();
            return accountId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                       : std::optional<std::string>{accountId.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentMailboxId(const QTreeView& mailboxView)
        {
            const auto currentIndex = mailboxView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto mailboxId =
                currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole)
                    .toString();
            // Empty mailbox id means an account-level node is selected.
            return mailboxId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                       : std::optional<std::string>{mailboxId.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentMailboxRole(const QTreeView& mailboxView)
        {
            const auto currentIndex = mailboxView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto role =
                currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxRoleRole)
                    .toString();
            return role.isEmpty() ? std::optional<std::string>{std::nullopt}
                                  : std::optional<std::string>{role.toStdString()};
        }

        [[nodiscard]] std::vector<javelin::protocol::MailboxSelectionSettings>
        withoutMailboxSelection(std::vector<javelin::protocol::MailboxSelectionSettings> selections,
                                const QStringView accountId, const QStringView mailboxId)
        {
            const auto account =
                std::ranges::find(selections, accountId.toString(),
                                  &javelin::protocol::MailboxSelectionSettings::accountId);
            if (account == selections.end())
                return selections;
            std::erase(account->mailboxIds, mailboxId.toString());
            return selections;
        }

        [[nodiscard]] bool indexIsUnread(const QModelIndex& index)
        {
            return index.isValid() &&
                   index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool();
        }

        using javelin::gui::mailboxes::findMailboxIndexForSelection;

        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::MailboxReader& mailboxReader,
                          const std::string_view accountId, const std::string_view role)
        {
            const auto result = mailboxReader.listMailboxTree(accountId);
            const auto* mailboxes =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
            if (mailboxes == nullptr)
            {
                return std::nullopt;
            }

            for (const auto& mailbox : *mailboxes)
            {
                if (mailbox.role == std::optional<std::string>{std::string{role}})
                {
                    return mailbox;
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] QString
        sortPropertyLabel(const javelin::jmap::query::EmailListSortProperty property)
        {
            switch (property)
            {
            case javelin::jmap::query::EmailListSortProperty::ReceivedAt:
                return i18n("Date received");
            case javelin::jmap::query::EmailListSortProperty::SentAt:
                return i18n("Date sent");
            case javelin::jmap::query::EmailListSortProperty::From:
                return i18nc("@item message sort property", "From");
            case javelin::jmap::query::EmailListSortProperty::To:
                return i18nc("@item message sort property", "To");
            case javelin::jmap::query::EmailListSortProperty::Subject:
                return i18nc("@item message sort property", "Subject");
            case javelin::jmap::query::EmailListSortProperty::Size:
                return i18nc("@item message sort property", "Size");
            }

            return i18n("Date received");
        }

        [[nodiscard]] QString
        sortDirectionLabel(const javelin::jmap::query::EmailListSortDirection direction)
        {
            return direction == javelin::jmap::query::EmailListSortDirection::Ascending
                       ? i18nc("@item message sort direction", "ascending")
                       : i18nc("@item message sort direction", "descending");
        }

    } // namespace

    MainWindow::MainWindow(javelin::gui::settings::GuiSettings& settings,
                           javelin::app::AccountCommandPort& accountCommandPort,
                           javelin::jmap::cache::AccountReader& accountReader,
                           javelin::jmap::cache::MailboxReader& mailboxReader,
                           javelin::jmap::cache::MailTagReader& mailTagReader,
                           javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
                           javelin::jmap::cache::IdentityReader& identityReader,
                           javelin::jmap::cache::MessageViewReader& messageViewReader,
                           QString databasePath,
                           javelin::gui::translation::TranslationService& translationService,
                           javelin::app::DeveloperDiagnosticsPort& developerDiagnosticsPort,
                           javelin::app::DeveloperMaintenancePort& developerMaintenancePort,
                           javelin::app::DaemonLogPort& daemonLogPort,
                           javelin::app::MailCommandPort& mailCommandPort,
                           javelin::app::MailExportPort& mailExportPort,
                           javelin::app::SieveCommandPort& sieveCommandPort,
                           javelin::app::IdentityCommandPort& identityCommandPort,
                           javelin::app::AccountRefreshPort& accountRefreshPort,
                           javelin::app::OnboardingPort& onboardingPort,
                           javelin::app::MessageContentPort& messageContentPort,
                           javelin::app::MessageListSessionFactoryPort& messageListSessionFactory,
                           javelin::app::MailApplicationEventsPort& mailEvents,
                           javelin::app::MessageNavigationPort& messageNavigationPort,
                           javelin::app::UndoCommandPort& undoCommandPort,
                           MainWindowFeatureFactories featureFactories, QWidget* parent)
        : KXmlGuiWindow(parent), m_settings(settings), m_accountCommandPort(accountCommandPort),
          m_accountReader(accountReader), m_mailboxReader(mailboxReader),
          m_mailTagReader(mailTagReader), m_contactIdentityLookup(contactIdentityLookup),
          m_identityReader(identityReader), m_messageViewReader(messageViewReader),
          m_databasePath(std::move(databasePath)), m_translationService(translationService),
          m_developerDiagnosticsPort(developerDiagnosticsPort),
          m_developerMaintenancePort(developerMaintenancePort), m_daemonLogPort(daemonLogPort),
          m_mailCommandPort(mailCommandPort), m_sieveCommandPort(sieveCommandPort),
          m_identityCommandPort(identityCommandPort), m_accountRefreshPort(accountRefreshPort),
          m_onboardingPort(onboardingPort), m_messageContentPort(messageContentPort),
          m_mailEvents(mailEvents), m_messageNavigationPort(messageNavigationPort),
          m_undoCommandPort(undoCommandPort),
          m_mailWorkspaceController(std::make_unique<MailWorkspaceController>(
              messageListSessionFactory, messageWindowSize, this)),
          m_emailListSort(m_mailWorkspaceController->sort()),
          m_activeTabIndex(m_mailWorkspaceController->activeIndex()),
          m_tabs(m_mailWorkspaceController->tabs())
    {
        m_statusBar = new LayeredStatusBar(this);
        setStatusBar(m_statusBar);
        m_messageFileController = new MessageFileController(m_settings, m_messageContentPort,
                                                            m_messageViewReader, this, this);
        connect(m_messageFileController, &MessageFileController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                {
                    if (durationMilliseconds > 0)
                        m_statusBar->showMessage(message, durationMilliseconds);
                    else
                        m_statusBar->showMessage(message);
                });
        connect(m_messageFileController, &MessageFileController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(m_messageFileController, &MessageFileController::userInterventionRequired, this,
                &MainWindow::presentUserInterventionError);
        m_mailExportController = new MailExportController(mailExportPort, m_accountReader,
                                                          m_mailboxReader, m_settings, *this, this);
        connect(m_mailExportController, &MailExportController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_mailExportController, &MailExportController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        setupUi();
        connect(&m_undoCommandPort, &javelin::app::UndoCommandPort::historyStateChanged, this,
                [this](const javelin::app::undo::HistoryState&) { updateUndoRedoActions(); });
        connect(
            &m_undoCommandPort, &javelin::app::UndoCommandPort::executionCompleted, this,
            [this](const QString& entryId, const javelin::app::undo::HistoryRefreshScope&)
            {
                refreshViewsFromCache();
                const auto& entries = m_undoCommandPort.entries();
                const auto completed =
                    std::ranges::find(entries, entryId, &javelin::app::undo::HistoryEntry::entryId);
                if (completed == entries.end())
                    return;
                m_statusBar->showMessage(completed->stack == javelin::app::undo::HistoryStack::Redo
                                             ? i18n("Undid %1.", completed->label)
                                             : i18n("Redid %1.", completed->label),
                                         5000);
            });
        connect(&m_undoCommandPort, &javelin::app::UndoCommandPort::executionFailed, this,
                &MainWindow::presentHistoryFailure);
        connect(qApp, &QApplication::focusChanged, this,
                [this](QWidget*, QWidget*) { updateUndoRedoActions(); });
        qApp->installEventFilter(this);
        updateUndoRedoActions();
        m_accountRefreshController = new AccountRefreshController(
            m_settings, m_accountRefreshPort, m_accountReader, m_mailboxReader, this);
        m_authenticationPromptCoordinator = new AuthenticationPromptCoordinator(
            m_settings, m_onboardingPort, *m_accountRefreshController, *this, this);
        connect(m_accountRefreshController, &AccountRefreshController::busyChanged, this,
                [this](const bool busy)
                {
                    m_refreshAction->setEnabled(!busy);
                    m_preferencesAction->setEnabled(!busy);
                });
        connect(m_accountRefreshController, &AccountRefreshController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                {
                    if (durationMilliseconds > 0)
                        m_statusBar->showMessage(message, durationMilliseconds);
                    else
                        m_statusBar->showMessage(message);
                });
        connect(m_accountRefreshController, &AccountRefreshController::userInterventionRequired,
                this, &MainWindow::presentUserInterventionError);
        connect(m_accountRefreshController, &AccountRefreshController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(m_accountRefreshController, &AccountRefreshController::accountRefreshed, this,
                [this](const javelin::jmap::LiveRefreshSummary& summary)
                {
                    if (summary.selectedMailboxId.has_value())
                        markTabsStaleForAccount(summary.accountId,
                                                std::string_view{*summary.selectedMailboxId});
                    else
                        markTabsStaleForAccount(summary.accountId);
                    if (m_tabs.empty() && !currentMailboxId(*m_mailboxView).has_value())
                        m_pendingInitialMailboxAccountId = summary.accountId;
                    reloadAccounts();
                    loadActiveTabFromCache(true);
                    selectPendingInitialMailbox();
                    const auto account =
                        m_settings.accountForCachedId(QString::fromStdString(summary.accountId));
                    const auto accountName =
                        account.displayName.isEmpty() ? i18n("this account") : account.displayName;
                    m_statusBar->showMessage(i18n("Synced %1 mailboxes and %2 messages for %3.",
                                                  summary.mailboxCount, summary.emailCount,
                                                  accountName),
                                             10000);
                });
        connect(m_accountRefreshController, &AccountRefreshController::contactsRefreshed, this,
                [this](const javelin::jmap::contacts::ContactRefreshSummary&)
                { reloadAccounts(); });
        m_calendarTabController = featureFactories.calendar(*m_contentStack, m_tabs, this);
        connect(m_calendarTabController, &CalendarTabController::tabReady, this,
                [this](const int index)
                {
                    m_mailWorkspaceController->setActiveIndex(index);
                    updateTabBar();
                    activateTab(index, false);
                });
        connect(m_calendarTabController, &CalendarTabController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_calendarTabController, &CalendarTabController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        m_contactsTabController = featureFactories.contacts(*m_contentStack, m_tabs, this);
        connect(m_contactsTabController, &ContactsTabController::tabReady, this,
                [this](const int index)
                {
                    m_mailWorkspaceController->setActiveIndex(index);
                    updateTabBar();
                    activateTab(index, false);
                });
        connect(m_contactsTabController, &ContactsTabController::toolbarStateChanged, this,
                &MainWindow::updateActiveContextUi);
        connect(m_contactsTabController, &ContactsTabController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_contactsTabController, &ContactsTabController::userInterventionRequired, this,
                &MainWindow::presentUserInterventionError);
        connect(m_contactsTabController, &ContactsTabController::composeMailRequested, this,
                [this](const QString& accountId, const QString& name, const QString& email)
                {
                    openComposeForRequest({
                        .accountId = accountId.toStdString(),
                        .mode = javelin::jmap::submission::ComposeMode::NewMessage,
                        .referenceEmailId = std::nullopt,
                        .draftEmailId = std::nullopt,
                        .initialTo = {{.name = name.isEmpty()
                                                   ? std::nullopt
                                                   : std::optional<std::string>{name.toStdString()},
                                       .email = email.toStdString()}},
                    });
                });
        connect(m_contactsTabController, &ContactsTabController::searchMailFromRequested, this,
                [this](const QString& accountId, const QString& email)
                {
                    const auto normalizedAccountId = accountId.trimmed();
                    const auto normalizedEmail = email.trimmed();
                    if (normalizedAccountId.isEmpty() || normalizedEmail.isEmpty())
                        return;
                    openOrActivateSearchTab(normalizedAccountId.toStdString(),
                                            javelin::jmap::search::EmailSearchCriteria{
                                                .from = normalizedEmail.toStdString()},
                                            true);
                });
        m_composeTabController = featureFactories.compose(*m_contentStack, m_tabs, this);
        connect(m_composeTabController, &ComposeTabController::tabReady, this,
                [this](const int index)
                {
                    m_mailWorkspaceController->setActiveIndex(index);
                    updateTabBar();
                    activateTab(index, false);
                });
        connect(m_composeTabController, &ComposeTabController::tabBarChanged, this,
                &MainWindow::updateTabBar);
        connect(m_composeTabController, &ComposeTabController::closeRequested, this,
                &MainWindow::closeTab);
        connect(m_composeTabController, &ComposeTabController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_composeTabController, &ComposeTabController::userInterventionRequired, this,
                &MainWindow::presentUserInterventionError);
        connect(m_composeTabController, &ComposeTabController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(m_composeTabController, &ComposeTabController::toolbarStateChanged, this,
                &MainWindow::updateActiveContextUi);
        connect(m_composeTabController, &ComposeTabController::manageIdentitiesRequested, this,
                &MainWindow::openSendingIdentitiesFor);
        m_messageListTabBindingPresenter = std::make_unique<MessageListTabBindingPresenter>(
            *m_mailboxModel, *m_mailboxView, *m_mailboxSearchEdit, *m_messageModel, *m_mailboxPane);
        m_messageSelectionController = std::make_unique<MessageSelectionController>(
            *m_mailboxModel, *m_mailboxView, *m_messageModel, *m_messageView);
        m_messageNavigationController = std::make_unique<MessageNavigationController>(
            m_messageNavigationPort, m_mailWorkspaceController->messageListTabs());
        m_messageContentController = new MessageContentController(m_messageContentPort, this);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::contentRequired,
                m_messageContentController,
                [this](const QString& accountId, const QString& emailId)
                {
                    m_messageContentController->request(accountId.toStdString(),
                                                        emailId.toStdString());
                });
        connect(m_messageContentController, &MessageContentController::contentUnavailable, this,
                [this](const javelin::jmap::MessageContentUnavailable& unavailable)
                {
                    const auto message =
                        i18n("%1 Refreshing the current view…", unavailable.message);
                    m_messageViewContainer->setErrorState(message);
                    m_statusBar->showMessage(message, 10000);
                });
        connect(m_messageContentController, &MessageContentController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error)
                {
                    m_messageViewContainer->setErrorState(error.message);
                    presentError(error);
                });
        connect(m_messageContentController, &MessageContentController::contentRefreshed, this,
                [this](const javelin::jmap::MessageContentRefreshSummary& summary)
                {
                    const auto currentAccount = activeAccountId();
                    const auto selectedEmails = m_messageSelectionController->selectedEmailIds();
                    const auto* route = m_messageNavigationController->activeRoute(activeTab());
                    const auto currentAccountView =
                        currentAccount.has_value()
                            ? std::optional<std::string_view>{*currentAccount}
                            : std::optional<std::string_view>{std::nullopt};
                    const auto routeAccountId =
                        route != nullptr ? std::optional<std::string_view>{route->accountId}
                                         : std::optional<std::string_view>{std::nullopt};
                    const auto routeEmailId = route != nullptr
                                                  ? std::optional<std::string_view>{route->emailId}
                                                  : std::optional<std::string_view>{std::nullopt};
                    if (!ownsMessageContentResult({
                            .requestAccountId = summary.accountId,
                            .requestEmailId = summary.emailId,
                            .activeAccountId = currentAccountView,
                            .selectedEmailIds = selectedEmails,
                            .routeAccountId = routeAccountId,
                            .routeEmailId = routeEmailId,
                        }))
                    {
                        return;
                    }

                    m_messageViewContainer->refresh(m_messageViewReader);
                    updateEmptyStates();
                    updateMessageListHeader();
                    m_mailActionController->update();
                    if (!summary.usedCachedContent)
                        m_statusBar->showMessage(i18n("Message ready."), 5000);
                });
        connect(m_mailWorkspaceController.get(), &MailWorkspaceController::stateChanged, this,
                [this](javelin::app::MessageListSession* session)
                {
                    if (!m_mailWorkspaceController->ownsActiveSession(session))
                        return;

                    applyActiveTabItemsPreservingSelection(
                        m_messageSelectionController->currentRow());
                    updateEmptyStates();
                    updateMessageListHeader();
                    m_quickFilterController->activate(activeTab());
                    resolveOpenEmailRoute();
                    QTimer::singleShot(0, this, &MainWindow::maybeLoadMoreMessages);
                });
        connect(m_mailWorkspaceController.get(), &MailWorkspaceController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(&m_mailEvents, &javelin::app::MailApplicationEventsPort::sessionCapabilitiesChanged,
                this,
                [this](const QString&)
                {
                    reloadAccounts();
                    m_calendarTabController->accountsChanged();
                });
        createActions();
        setupGUI(KXmlGuiWindow::ToolBar | KXmlGuiWindow::Keys | KXmlGuiWindow::Save |
                     KXmlGuiWindow::Create,
                 QStringLiteral("javelinmailui.rc"));
        m_emailContextMenu = qobject_cast<QMenu*>(
            guiFactory()->container(QStringLiteral("email_context_menu"), this));
        if (m_emailContextMenu != nullptr)
        {
            m_mailActionController->configureContextMenu(
                *m_emailContextMenu,
                [this] { return m_settings.workspaceSettings().emailContextMenuLayout; },
                [this](const QList<QAction*>& actions)
                {
                    unplugActionList(QStringLiteral("email_context_menu_layout"));
                    if (!actions.empty())
                        plugActionList(QStringLiteral("email_context_menu_layout"), actions);
                });
        }
        m_calendarTabController->configureEventContextMenu(
            [this] { return m_settings.workspaceSettings().calendarEventContextMenuLayout; },
            {.edit = *m_calendarEventEditAction,
             .duplicate = *m_calendarEventDuplicateAction,
             .move = *m_calendarEventMoveAction,
             .accept = *m_calendarEventAcceptAction,
             .tentative = *m_calendarEventTentativeAction,
             .decline = *m_calendarEventDeclineAction,
             .copyDetails = *m_calendarEventCopyDetailsAction,
             .deleteEvent = *m_calendarEventDeleteAction});
        if (auto* composeToolBar = toolBar(QStringLiteral("composeToolBar"));
            composeToolBar != nullptr)
        {
            if (auto* signatureButton = qobject_cast<QToolButton*>(
                    composeToolBar->widgetForAction(m_composeSignatureAction)))
                signatureButton->setPopupMode(QToolButton::MenuButtonPopup);
        }
        updateActiveContextUi();
        connectSelection();
        connect(&m_messageNavigationPort, &javelin::app::MessageNavigationPort::routeRequested,
                this, &MainWindow::openEmailRoute);
        const auto applyAccountStatus = [this](const QString& accountId, const auto status)
        {
            using Model = javelin::gui::mailboxes::MailboxTreeModel;
            Model::ConnectionStatus modelStatus = Model::ConnectionStatus::Disconnected;
            if (status == javelin::app::MailAccountStatus::Connecting)
            {
                modelStatus = Model::ConnectionStatus::Connecting;
            }
            else if (status == javelin::app::MailAccountStatus::Connected)
            {
                modelStatus = Model::ConnectionStatus::Connected;
            }
            else if (status == javelin::app::MailAccountStatus::AuthenticationPaused)
            {
                modelStatus = Model::ConnectionStatus::AuthenticationPaused;
            }
            m_accountStatuses[accountId.toStdString()] = status;
            m_mailboxModel->setConnectionStatus(accountId, modelStatus);
            m_authenticationPromptCoordinator->updateAccountStatus(accountId, status);
            updateEmptyStates();
        };
        connect(&m_mailEvents, &javelin::app::MailApplicationEventsPort::accountStatusChanged, this,
                applyAccountStatus);
        for (const auto& [accountId, status] : m_mailEvents.accountStatuses())
            applyAccountStatus(QString::fromStdString(accountId), status);
        connect(
            &m_mailEvents, &javelin::app::MailApplicationEventsPort::cacheInvalidated, this,
            [this](const javelin::app::MailCacheInvalidation& invalidation)
            {
                const auto& change = invalidation.change;
                if (!change.messageContentEmailIds.empty() &&
                    activeAccountId() == std::optional<std::string>{change.accountId.toStdString()})
                {
                    const auto selectedEmails = m_messageSelectionController->selectedEmailIds();
                    const auto* route = m_messageNavigationController->activeRoute(activeTab());
                    const bool hydratesSelection =
                        std::ranges::any_of(change.messageContentEmailIds,
                                            [&selectedEmails, route](const QString& emailId)
                                            {
                                                const auto id = emailId.toStdString();
                                                return std::ranges::contains(selectedEmails, id) ||
                                                       (route != nullptr && route->emailId == id);
                                            });
                    if (hydratesSelection)
                    {
                        m_messageViewContainer->refresh(m_messageViewReader);
                        updateEmptyStates();
                    }
                }
                if (change.mailboxTreeChanged)
                {
                    QSignalBlocker mailboxSelectionBlocker{m_mailboxView->selectionModel()};
                    if (m_mailboxModel->refreshAccount(change.accountId))
                        m_mailboxView->expandAll();
                }

                for (const auto& mailboxId : change.mailboxIds)
                {
                    const auto mailbox = mailboxId.toStdString();
                    if (change.hasNewMail && activeTabIsMailbox() &&
                        activeAccountId() ==
                            std::optional<std::string>{change.accountId.toStdString()} &&
                        activeMailboxId() == std::optional<std::string>{mailbox} &&
                        m_messageModel->rowCount() > 0 &&
                        m_messageView->verticalScrollBar()->value() == 0)
                    {
                        m_messageView->scrollTo(m_messageModel->index(0, 0));
                    }
                }
                resolveOpenEmailRoute();
            });
        restorePersistentState();

        const auto accounts = m_settings.accounts();
        const auto uninitialized =
            std::ranges::find_if(accounts, javelin::gui::settings::needsInitialAccountBootstrap);
        if (uninitialized != accounts.end())
        {
            QTimer::singleShot(0, this, [this, settings = *uninitialized]
                               { m_accountRefreshController->refreshConnection(settings); });
        }

        auto* stateSaveTimer = new QTimer(this);
        stateSaveTimer->setInterval(std::chrono::minutes{1});
        connect(stateSaveTimer, &QTimer::timeout, this, &MainWindow::savePersistentState);
        stateSaveTimer->start();
    }

    struct MainWindow::ClosedTabState
    {
        PersistedTab tab;
    };

    MainWindow::~MainWindow() = default;

    void MainWindow::openEmailRoute(const javelin::app::OpenEmailRoute& route)
    {
        m_messageCommandController->markEmailRead(route.accountId, route.emailId);

        const auto accountId = QString::fromStdString(route.accountId);
        const auto mailboxId = QString::fromStdString(route.mailboxId);

        reloadAccounts();
        QString mailboxTitle = mailboxId;
        if (route.mailboxName.has_value() && !route.mailboxName->empty())
            mailboxTitle = QString::fromStdString(*route.mailboxName);
        std::optional<std::string> mailboxRole;
        const auto mailboxIndex = findMailboxIndexForSelection(*m_mailboxModel, accountId,
                                                               std::optional<QString>{mailboxId});
        if (mailboxIndex.isValid())
        {
            const auto cachedMailboxTitle =
                mailboxIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxNameRole)
                    .toString();
            if (!cachedMailboxTitle.isEmpty())
                mailboxTitle = cachedMailboxTitle;
            const auto role =
                mailboxIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxRoleRole)
                    .toString();
            if (!role.isEmpty())
                mailboxRole = role.toStdString();
        }

        activateMailboxInHomeTab(route.accountId, route.mailboxId, mailboxTitle, mailboxRole,
                                 false);
        if (auto* tab = activeTab())
            m_messageNavigationController->begin(*tab, route);
        loadActiveTabFromCache(true, false);
        resolveOpenEmailRoute();
    }

    void MainWindow::resolveOpenEmailRoute()
    {
        const auto resolution = m_messageNavigationController->resolve(
            activeTab(), m_messageSelectionController->rowIdentities());
        if (!resolution.route.has_value())
            return;

        const auto& route = *resolution.route;
        if (resolution.currentRow.has_value())
        {
            const auto index = m_messageModel->index(static_cast<int>(*resolution.currentRow), 0);
            if (index.isValid())
            {
                const auto rowEmailId =
                    index.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
                if (m_messageSelectionController->selectMessageAlone(rowEmailId))
                {
                    m_messageSelectionController->syncTabSelection(activeTab());
                    if (resolution.completeRoute &&
                        m_messageSelectionController->currentEmailId() ==
                            std::optional<std::string>{route.emailId})
                    {
                        m_messageNavigationController->complete(route.id);
                    }
                }
            }
        }

        setMessageViewSelection(route.accountId, route.mailboxId, route.emailId);
    }

    void MainWindow::createActions()
    {
        const auto iconColor = palette().color(QPalette::Text);
        auto thunderbirdIcon = [iconColor](const QString& resourcePath)
        { return javelin::gui::themedSvgIcon(resourcePath, iconColor); };

        m_undoAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-undo")),
                                   i18nc("@action", "Undo"), this);
        connect(m_undoAction, &QAction::triggered, this, &MainWindow::routeUndo);
        actionCollection()->addAction(QStringLiteral("undo_operation"), m_undoAction);
        actionCollection()->setDefaultShortcut(m_undoAction, QKeySequence::Undo);

        m_redoAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-redo")),
                                   i18nc("@action", "Redo"), this);
        connect(m_redoAction, &QAction::triggered, this, &MainWindow::routeRedo);
        actionCollection()->addAction(QStringLiteral("redo_operation"), m_redoAction);
        auto redoShortcuts = QKeySequence::keyBindings(QKeySequence::Redo);
        const QKeySequence controlShiftZ{Qt::CTRL | Qt::SHIFT | Qt::Key_Z};
        if (!redoShortcuts.contains(controlShiftZ))
            redoShortcuts.prepend(controlShiftZ);
        actionCollection()->setDefaultShortcuts(m_redoAction, redoShortcuts);

        m_closeTabAction = new QAction(QIcon::fromTheme(QStringLiteral("tab-close")),
                                       i18nc("@action", "Close Tab"), this);
        connect(m_closeTabAction, &QAction::triggered, this,
                [this]
                {
                    if (m_activeTabIndex.has_value())
                        closeTab(*m_activeTabIndex);
                });
        actionCollection()->addAction(QStringLiteral("close_tab"), m_closeTabAction);
        actionCollection()->setDefaultShortcuts(m_closeTabAction,
                                                QKeySequence::keyBindings(QKeySequence::Close));

        m_previousTabAction = new QAction(QIcon::fromTheme(QStringLiteral("go-previous")),
                                          i18nc("@action", "Previous Tab"), this);
        connect(m_previousTabAction, &QAction::triggered, this,
                [this] { activateRelativeTab(-1); });
        actionCollection()->addAction(QStringLiteral("previous_tab"), m_previousTabAction);
        actionCollection()->setDefaultShortcuts(
            m_previousTabAction, QKeySequence::keyBindings(QKeySequence::PreviousChild));

        m_nextTabAction = new QAction(QIcon::fromTheme(QStringLiteral("go-next")),
                                      i18nc("@action", "Next Tab"), this);
        connect(m_nextTabAction, &QAction::triggered, this, [this] { activateRelativeTab(1); });
        actionCollection()->addAction(QStringLiteral("next_tab"), m_nextTabAction);
        actionCollection()->setDefaultShortcuts(m_nextTabAction,
                                                QKeySequence::keyBindings(QKeySequence::NextChild));

        m_reopenClosedTabAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-undo")),
                                              i18nc("@action", "Reopen Closed Tab"), this);
        connect(m_reopenClosedTabAction, &QAction::triggered, this,
                &MainWindow::reopenLastClosedTab);
        actionCollection()->addAction(QStringLiteral("reopen_closed_tab"), m_reopenClosedTabAction);
        actionCollection()->setDefaultShortcut(m_reopenClosedTabAction,
                                               QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_T});
        m_reopenClosedTabAction->setEnabled(false);

        m_previousMessageAction = new QAction(QIcon::fromTheme(QStringLiteral("go-up")),
                                              i18nc("@action", "Previous Message"), this);
        connect(m_previousMessageAction, &QAction::triggered, this,
                [this] { moveMessageSelection(-1, false); });
        actionCollection()->addAction(QStringLiteral("previous_message"), m_previousMessageAction);
        actionCollection()->setDefaultShortcut(m_previousMessageAction,
                                               QKeySequence{Qt::ALT | Qt::Key_Up});

        m_nextMessageAction = new QAction(QIcon::fromTheme(QStringLiteral("go-down")),
                                          i18nc("@action", "Next Message"), this);
        connect(m_nextMessageAction, &QAction::triggered, this,
                [this] { moveMessageSelection(1, false); });
        actionCollection()->addAction(QStringLiteral("next_message"), m_nextMessageAction);
        actionCollection()->setDefaultShortcut(m_nextMessageAction,
                                               QKeySequence{Qt::ALT | Qt::Key_Down});

        m_previousUnreadMessageAction =
            new QAction(QIcon::fromTheme(QStringLiteral("mail-unread")),
                        i18nc("@action", "Previous Unread Message"), this);
        connect(m_previousUnreadMessageAction, &QAction::triggered, this,
                [this] { moveMessageSelection(-1, true); });
        actionCollection()->addAction(QStringLiteral("previous_unread_message"),
                                      m_previousUnreadMessageAction);
        actionCollection()->setDefaultShortcut(m_previousUnreadMessageAction,
                                               QKeySequence{Qt::CTRL | Qt::ALT | Qt::Key_Up});

        m_nextUnreadMessageAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-unread")),
                                                i18nc("@action", "Next Unread Message"), this);
        connect(m_nextUnreadMessageAction, &QAction::triggered, this,
                [this] { moveMessageSelection(1, true); });
        actionCollection()->addAction(QStringLiteral("next_unread_message"),
                                      m_nextUnreadMessageAction);
        actionCollection()->setDefaultShortcut(m_nextUnreadMessageAction,
                                               QKeySequence{Qt::CTRL | Qt::ALT | Qt::Key_Down});

        m_focusMailboxTreeAction =
            new QAction(QIcon::fromTheme(QStringLiteral("mail-folder-inbox")),
                        i18nc("@action", "Focus Mailbox Tree"), this);
        connect(m_focusMailboxTreeAction, &QAction::triggered, this,
                [this] { m_mailboxView->setFocus(Qt::ShortcutFocusReason); });
        actionCollection()->addAction(QStringLiteral("focus_mailbox_tree"),
                                      m_focusMailboxTreeAction);
        actionCollection()->setDefaultShortcut(m_focusMailboxTreeAction,
                                               QKeySequence{Qt::ALT | Qt::Key_1});

        m_focusMessageListAction =
            new QAction(QIcon::fromTheme(QStringLiteral("view-list-details")),
                        i18nc("@action", "Focus Message List"), this);
        connect(m_focusMessageListAction, &QAction::triggered, this,
                [this] { m_messageView->setFocus(Qt::ShortcutFocusReason); });
        actionCollection()->addAction(QStringLiteral("focus_message_list"),
                                      m_focusMessageListAction);
        actionCollection()->setDefaultShortcut(m_focusMessageListAction,
                                               QKeySequence{Qt::ALT | Qt::Key_2});

        m_focusMessageReaderAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-read")),
                                                 i18nc("@action", "Focus Message Reader"), this);
        connect(m_focusMessageReaderAction, &QAction::triggered, m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::focusMessageBody);
        actionCollection()->addAction(QStringLiteral("focus_message_reader"),
                                      m_focusMessageReaderAction);
        actionCollection()->setDefaultShortcut(m_focusMessageReaderAction,
                                               QKeySequence{Qt::ALT | Qt::Key_3});

        m_focusSearchAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-find")),
                                          i18nc("@action", "Focus Search Field"), this);
        connect(m_focusSearchAction, &QAction::triggered, this,
                [this]
                {
                    m_mailboxSearchEdit->setFocus(Qt::ShortcutFocusReason);
                    m_mailboxSearchEdit->selectAll();
                });
        actionCollection()->addAction(QStringLiteral("focus_search_field"), m_focusSearchAction);
        actionCollection()->setDefaultShortcut(m_focusSearchAction,
                                               QKeySequence{Qt::ALT | Qt::Key_0});

        m_saveCurrentAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                          i18nc("@action", "Save"), this);
        connect(m_saveCurrentAction, &QAction::triggered, this, &MainWindow::routeSaveCurrent);
        actionCollection()->addAction(QStringLiteral("save_current_item"), m_saveCurrentAction);
        actionCollection()->setDefaultShortcut(m_saveCurrentAction, QKeySequence::Save);

        m_exportMailboxAction = new QAction(QIcon::fromTheme(QStringLiteral("document-export")),
                                            i18n("Mailbox…"), this);
        connect(m_exportMailboxAction, &QAction::triggered, this,
                &MainWindow::exportCurrentMailbox);
        actionCollection()->addAction(QStringLiteral("export_mailbox"), m_exportMailboxAction);

        m_exportAccountAction = new QAction(QIcon::fromTheme(QStringLiteral("document-export")),
                                            i18n("Account…"), this);
        connect(m_exportAccountAction, &QAction::triggered, this,
                &MainWindow::exportCurrentAccount);
        actionCollection()->addAction(QStringLiteral("export_account"), m_exportAccountAction);

        m_refreshAction = new QAction(
            thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/cloud-download.svg")),
            i18n("Refresh From Server"), this);
        m_refreshAction->setShortcut(QKeySequence::Refresh);
        connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshFromServer);
        actionCollection()->addAction(QStringLiteral("refresh_from_server"), m_refreshAction);
        actionCollection()->setDefaultShortcut(m_refreshAction, QKeySequence::Refresh);

        m_quitAction = new QAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                   i18nc("@action:inmenu", "&Quit"), this);
        m_quitAction->setShortcut(QKeySequence::Quit);
        connect(m_quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);
        actionCollection()->addAction(QStringLiteral("quit_application"), m_quitAction);
        actionCollection()->setDefaultShortcut(m_quitAction, QKeySequence::Quit);

        m_preferencesAction =
            KStandardAction::preferences(this, &MainWindow::openPreferences, actionCollection());
        m_preferencesAction->setIcon(
            thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/settings.svg")));

        m_configureEmailContextMenuAction =
            new QAction(QIcon::fromTheme(QStringLiteral("configure")),
                        i18n("Configure Email Context Menu…"), this);
        connect(m_configureEmailContextMenuAction, &QAction::triggered, this,
                &MainWindow::configureEmailContextMenu);
        actionCollection()->addAction(QStringLiteral("configure_email_context_menu"),
                                      m_configureEmailContextMenuAction);

        m_configureCalendarEventContextMenuAction =
            new QAction(QIcon::fromTheme(QStringLiteral("configure")),
                        i18n("Configure Calendar Event Context Menu…"), this);
        connect(m_configureCalendarEventContextMenuAction, &QAction::triggered, this,
                &MainWindow::configureCalendarEventContextMenu);
        actionCollection()->addAction(QStringLiteral("configure_calendar_event_context_menu"),
                                      m_configureCalendarEventContextMenuAction);

        m_themeController = new ThemeController(
            *this, *m_quickFilterController, *m_messageSortButton,
            [this]
            {
                updateTabBar();
                m_calendarTabController->applicationPaletteChanged();
                m_contactsTabController->applicationPaletteChanged();
                m_mailboxView->viewport()->update();
                m_messageView->viewport()->update();
            },
            this);
        actionCollection()->addAction(QStringLiteral("toggle_dark_mode"),
                                      &m_themeController->darkModeAction());

        m_developerOptionsAction =
            new QAction(QIcon::fromTheme(QStringLiteral("applications-development")),
                        i18n("Developer Options…"), this);
        connect(m_developerOptionsAction, &QAction::triggered, this,
                &MainWindow::openDeveloperOptions);
        actionCollection()->addAction(QStringLiteral("open_developer_options"),
                                      m_developerOptionsAction);

        m_contactsAction = new QAction(QIcon::fromTheme(QStringLiteral("view-pim-contacts")),
                                       i18n("Contacts"), this);
        connect(m_contactsAction, &QAction::triggered, this, &MainWindow::openContacts);
        actionCollection()->addAction(QStringLiteral("open_contacts"), m_contactsAction);
        m_contactsAction->setEnabled(false);

        m_calendarAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-month")),
                                       i18n("Calendar"), this);
        connect(m_calendarAction, &QAction::triggered, this, &MainWindow::openCalendar);
        actionCollection()->addAction(QStringLiteral("open_calendar"), m_calendarAction);
        m_calendarAction->setEnabled(false);

        m_sieveAction = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                    i18n("Sieve Rules"), this);
        connect(m_sieveAction, &QAction::triggered, this, &MainWindow::openSieveEditor);
        actionCollection()->addAction(QStringLiteral("open_sieve_editor"), m_sieveAction);

        m_sendingIdentitiesAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-identity")),
                                                i18n("Sending Identities and Signatures…"), this);
        connect(m_sendingIdentitiesAction, &QAction::triggered, this,
                &MainWindow::openSendingIdentities);
        actionCollection()->addAction(QStringLiteral("open_sending_identities"),
                                      m_sendingIdentitiesAction);

        m_newMessageAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/new-mail.svg")),
                        i18nc("@action", "&New Message"), this);
        m_newMessageAction->setShortcut(QKeySequence::New);
        connect(m_newMessageAction, &QAction::triggered, this, &MainWindow::composeNewMessage);
        actionCollection()->addAction(QStringLiteral("compose_new_message"), m_newMessageAction);
        actionCollection()->setDefaultShortcut(m_newMessageAction, QKeySequence::New);

        m_replyAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/reply.svg")),
                        i18nc("@action", "&Reply"), this);
        m_replyAction->setShortcut(QKeySequence{Qt::Key_R});
        connect(m_replyAction, &QAction::triggered, this, &MainWindow::composeReply);
        actionCollection()->addAction(QStringLiteral("compose_reply"), m_replyAction);
        actionCollection()->setDefaultShortcut(m_replyAction, QKeySequence{Qt::Key_R});

        m_replyAllAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/reply-all.svg")),
                        i18nc("@action", "Reply &All"), this);
        connect(m_replyAllAction, &QAction::triggered, this, &MainWindow::composeReplyAll);
        actionCollection()->addAction(QStringLiteral("compose_reply_all"), m_replyAllAction);

        m_forwardAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/forward.svg")),
                        i18nc("@action", "&Forward"), this);
        connect(m_forwardAction, &QAction::triggered, this, &MainWindow::composeForward);
        actionCollection()->addAction(QStringLiteral("compose_forward"), m_forwardAction);

        m_editDraftAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/draft.svg")),
                        i18nc("@action", "Edit &Draft"), this);
        connect(m_editDraftAction, &QAction::triggered, this, &MainWindow::editSelectedDraft);
        actionCollection()->addAction(QStringLiteral("compose_edit_draft"), m_editDraftAction);

        m_archiveAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/archive.svg")),
                        i18nc("@action", "&Archive"), this);
        m_archiveAction->setShortcut(QKeySequence{Qt::Key_A});
        actionCollection()->addAction(QStringLiteral("archive_email"), m_archiveAction);
        actionCollection()->setDefaultShortcut(m_archiveAction, QKeySequence{Qt::Key_A});

        m_markUnreadAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/unread.svg")),
                        i18nc("@action", "Mark &Unread"), this);
        actionCollection()->addAction(QStringLiteral("mark_email_unread"), m_markUnreadAction);

        m_starAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/star.svg")),
                        i18nc("@action", "&Star"), this);
        m_starAction->setShortcut(QKeySequence{Qt::Key_S});
        actionCollection()->addAction(QStringLiteral("toggle_email_starred"), m_starAction);
        actionCollection()->setDefaultShortcut(m_starAction, QKeySequence{Qt::Key_S});

        m_junkAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/spam.svg")),
                        i18nc("@action", "&Junk"), this);
        actionCollection()->addAction(QStringLiteral("toggle_email_junk"), m_junkAction);

        m_tagsAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/tag.svg")),
                        i18nc("@action", "&Tags"), this);
        m_tagsMenu = new QMenu(this);
        m_tagsAction->setMenu(m_tagsMenu);
        actionCollection()->addAction(QStringLiteral("tag_email"), m_tagsAction);

        m_deleteAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/delete.svg")),
                        i18nc("@action", "&Delete"), this);
        m_deleteAction->setShortcut(QKeySequence::Delete);
        actionCollection()->addAction(QStringLiteral("delete_email"), m_deleteAction);
        actionCollection()->setDefaultShortcut(m_deleteAction, QKeySequence::Delete);

        m_permanentDeleteAction = new QAction(i18n("Delete Permanently"), this);
        actionCollection()->addAction(QStringLiteral("permanently_delete_email"),
                                      m_permanentDeleteAction);
        actionCollection()->setDefaultShortcut(m_permanentDeleteAction,
                                               QKeySequence{Qt::SHIFT | Qt::Key_Delete});

        m_moveAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-move")),
                                   i18nc("@action", "&Move to…"), this);
        m_moveMenu = new QMenu(this);
        m_moveAction->setMenu(m_moveMenu);
        actionCollection()->addAction(QStringLiteral("move_email"), m_moveAction);

        m_copyAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                   i18nc("@action", "&Copy to…"), this);
        m_copyMenu = new QMenu(this);
        m_copyAction->setMenu(m_copyMenu);
        actionCollection()->addAction(QStringLiteral("copy_email"), m_copyAction);

        m_viewSourceAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                         i18nc("@action", "View &Source"), this);
        connect(m_viewSourceAction, &QAction::triggered, this,
                &MainWindow::viewSelectedMessageSource);
        actionCollection()->addAction(QStringLiteral("view_message_source"), m_viewSourceAction);

        m_findSenderContextAction = new QAction(QIcon::fromTheme(QStringLiteral("system-search")),
                                                i18n("Find all conversations with sender"), this);
        connect(m_findSenderContextAction, &QAction::triggered, this,
                [this] { findConversationsWithSender(m_messageView->currentIndex()); });
        actionCollection()->addAction(QStringLiteral("find_conversations_with_sender"),
                                      m_findSenderContextAction);
        KActionCollection::setShortcutsConfigurable(m_findSenderContextAction, false);

        m_advancedSearchAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/search.svg")),
                        i18n("Advanced Search"), this);
        connect(m_advancedSearchAction, &QAction::triggered, this, &MainWindow::showAdvancedSearch);
        actionCollection()->addAction(QStringLiteral("advanced_search"), m_advancedSearchAction);

        m_composeSendAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-send")),
                                          i18nc("@action", "Send"), this);
        connect(m_composeSendAction, &QAction::triggered, this,
                [this] { m_composeTabController->sendMessage(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_send"), m_composeSendAction);

        m_composeScheduleSendAction =
            new QAction(QIcon::fromTheme(QStringLiteral("appointment-new")),
                        i18nc("@action", "Schedule Send…"), this);
        connect(m_composeScheduleSendAction, &QAction::triggered, this,
                [this] { m_composeTabController->scheduleMessage(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_schedule_send"),
                                      m_composeScheduleSendAction);

        m_composeSaveDraftAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                               i18n("Save Draft"), this);
        connect(m_composeSaveDraftAction, &QAction::triggered, this,
                [this] { m_composeTabController->saveDraft(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_save_draft"),
                                      m_composeSaveDraftAction);

        m_composeAttachFilesAction = new QAction(
            QIcon::fromTheme(QStringLiteral("mail-attachment")), i18n("Attach Files"), this);
        connect(m_composeAttachFilesAction, &QAction::triggered, this,
                [this] { m_composeTabController->attachFiles(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_attach_files"),
                                      m_composeAttachFilesAction);

        m_composeSignatureAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-signature")),
                                               i18n("Signature"), this);
        connect(m_composeSignatureAction, &QAction::triggered, this,
                [this] { m_composeTabController->editCurrentSignature(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_signature"),
                                      m_composeSignatureAction);

        m_composeRichTextAction =
            new QAction(QIcon::fromTheme(QStringLiteral("preferences-desktop-font")),
                        i18nc("@action:button compose mode", "Rich Text"), this);
        m_composeRichTextAction->setCheckable(true);
        m_composeRichTextAction->setToolTip(
            i18nc("@info:tooltip", "Toggle rich text editing mode"));
        connect(m_composeRichTextAction, &QAction::toggled, this, [this](const bool enabled)
                { m_composeTabController->setRichTextEnabled(activeTab(), enabled); });
        actionCollection()->addAction(QStringLiteral("compose_rich_text"), m_composeRichTextAction);

        const auto invokeContact = [this](const ContactsTabCommand command)
        { m_contactsTabController->invoke(activeTab(), command); };
        const auto invokeContactWorkspace = [this](const ContactsTabCommand command)
        { m_contactsTabController->invokeWorkspace(command); };
        m_contactNewAction = new QAction(QIcon::fromTheme(QStringLiteral("contact-new")),
                                         i18nc("@action", "Add"), this);
        connect(m_contactNewAction, &QAction::triggered, this, [invokeContactWorkspace]
                { invokeContactWorkspace(ContactsTabCommand::CreateContact); });
        auto* contactAddMenu = new QMenu(this);
        auto* newContact = contactAddMenu->addAction(
            QIcon::fromTheme(QStringLiteral("contact-new")), i18n("New Contact"));
        connect(newContact, &QAction::triggered, this, [invokeContactWorkspace]
                { invokeContactWorkspace(ContactsTabCommand::CreateContact); });
        auto* newGroup = contactAddMenu->addAction(QIcon::fromTheme(QStringLiteral("system-users")),
                                                   i18n("New Group"));
        connect(newGroup, &QAction::triggered, this, [invokeContactWorkspace]
                { invokeContactWorkspace(ContactsTabCommand::CreateGroup); });
        m_contactNewAction->setMenu(contactAddMenu);
        actionCollection()->addAction(QStringLiteral("contact_new"), m_contactNewAction);
        m_contactEditAction = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                          i18n("Edit Contact"), this);
        connect(m_contactEditAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::EditContact); });
        actionCollection()->addAction(QStringLiteral("contact_edit"), m_contactEditAction);
        m_contactDeleteAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                            i18n("Delete Contact"), this);
        connect(m_contactDeleteAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::DeleteContact); });
        actionCollection()->addAction(QStringLiteral("contact_delete"), m_contactDeleteAction);
        m_contactCopyAction =
            new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")), i18n("Copy Contact"), this);
        connect(m_contactCopyAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::CopyContact); });
        actionCollection()->addAction(QStringLiteral("contact_copy"), m_contactCopyAction);
        m_contactImportAction = new QAction(QIcon::fromTheme(QStringLiteral("document-import")),
                                            i18n("Import vCard…"), this);
        connect(m_contactImportAction, &QAction::triggered, this, [invokeContactWorkspace]
                { invokeContactWorkspace(ContactsTabCommand::ImportVCard); });
        actionCollection()->addAction(QStringLiteral("contact_import"), m_contactImportAction);
        m_contactExportAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                            i18n("Save Contact As…"), this);
        connect(m_contactExportAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::ExportVCard); });
        actionCollection()->addAction(QStringLiteral("contact_export"), m_contactExportAction);
        m_contactDuplicatesAction =
            new QAction(QIcon::fromTheme(QStringLiteral("merge")), i18n("Find Duplicates…"), this);
        connect(m_contactDuplicatesAction, &QAction::triggered, this, [invokeContactWorkspace]
                { invokeContactWorkspace(ContactsTabCommand::FindDuplicates); });
        actionCollection()->addAction(QStringLiteral("contact_duplicates"),
                                      m_contactDuplicatesAction);
        m_contactAddToGroupAction =
            new QAction(QIcon::fromTheme(QStringLiteral("list-add")), i18n("Add to Group"), this);
        m_contactAddToGroupMenu = new QMenu(this);
        connect(m_contactAddToGroupMenu, &QMenu::aboutToShow, this,
                [this]
                {
                    m_contactsTabController->populateAddToGroupMenu(activeTab(),
                                                                    *m_contactAddToGroupMenu);
                });
        m_contactAddToGroupAction->setMenu(m_contactAddToGroupMenu);
        actionCollection()->addAction(QStringLiteral("contact_add_to_group"),
                                      m_contactAddToGroupAction);
        m_contactRemoveFromGroupAction = new QAction(
            QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove from Group"), this);
        m_contactRemoveFromGroupMenu = new QMenu(this);
        connect(m_contactRemoveFromGroupMenu, &QMenu::aboutToShow, this,
                [this]
                {
                    m_contactsTabController->populateRemoveFromGroupMenu(
                        activeTab(), *m_contactRemoveFromGroupMenu);
                });
        m_contactRemoveFromGroupAction->setMenu(m_contactRemoveFromGroupMenu);
        actionCollection()->addAction(QStringLiteral("contact_remove_from_group"),
                                      m_contactRemoveFromGroupAction);
        m_contactManageAddressBooksAction =
            new QAction(QIcon::fromTheme(QStringLiteral("view-list-details")),
                        i18n("Manage Address Books…"), this);
        connect(m_contactManageAddressBooksAction, &QAction::triggered, this,
                [invokeContactWorkspace]
                { invokeContactWorkspace(ContactsTabCommand::ManageAddressBooks); });
        m_contactAddressBooksMenu = new QMenu(this);
        connect(m_contactAddressBooksMenu, &QMenu::aboutToShow, this,
                [this]
                {
                    m_contactsTabController->populateAddressBookMenu(activeTab(),
                                                                     *m_contactAddressBooksMenu);
                });
        m_contactManageAddressBooksAction->setMenu(m_contactAddressBooksMenu);
        actionCollection()->addAction(QStringLiteral("contact_manage_address_books"),
                                      m_contactManageAddressBooksAction);
        m_contactRefreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                             i18n("Refresh Contacts"), this);
        connect(m_contactRefreshAction, &QAction::triggered, this,
                [invokeContactWorkspace] { invokeContactWorkspace(ContactsTabCommand::Refresh); });
        actionCollection()->addAction(QStringLiteral("contact_refresh"), m_contactRefreshAction);

        const auto invokeCalendar = [this](const CalendarTabCommand command)
        { m_calendarTabController->invokeWorkspace(command); };
        m_calendarNewEventAction = new QAction(QIcon::fromTheme(QStringLiteral("appointment-new")),
                                               i18n("New Event"), this);
        connect(m_calendarNewEventAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::CreateEvent); });
        actionCollection()->addAction(QStringLiteral("calendar_new_event"),
                                      m_calendarNewEventAction);
        m_calendarPreviousMonthAction = new QAction(QIcon::fromTheme(QStringLiteral("go-previous")),
                                                    i18n("Previous Month"), this);
        connect(m_calendarPreviousMonthAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::PreviousMonth); });
        actionCollection()->addAction(QStringLiteral("calendar_previous_month"),
                                      m_calendarPreviousMonthAction);
        m_calendarTodayAction =
            new QAction(QIcon::fromTheme(QStringLiteral("go-jump-today")), i18n("Today"), this);
        connect(m_calendarTodayAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::Today); });
        actionCollection()->addAction(QStringLiteral("calendar_today"), m_calendarTodayAction);
        m_calendarNextMonthAction =
            new QAction(QIcon::fromTheme(QStringLiteral("go-next")), i18n("Next Month"), this);
        connect(m_calendarNextMonthAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::NextMonth); });
        actionCollection()->addAction(QStringLiteral("calendar_next_month"),
                                      m_calendarNextMonthAction);
        m_calendarListAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-list")),
                                           i18n("Calendars"), this);
        connect(m_calendarListAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::ManageCalendars); });
        actionCollection()->addAction(QStringLiteral("calendar_list"), m_calendarListAction);
        m_calendarRefreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                              i18n("Refresh Calendar"), this);
        connect(m_calendarRefreshAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::Refresh); });
        actionCollection()->addAction(QStringLiteral("calendar_refresh"), m_calendarRefreshAction);

        const auto addCalendarEventAction =
            [this](QAction*& target, const QString& id, const QString& icon, const QString& text)
        {
            target = new QAction(QIcon::fromTheme(icon), text, this);
            target->setEnabled(false);
            actionCollection()->addAction(id, target);
            KActionCollection::setShortcutsConfigurable(target, false);
        };
        addCalendarEventAction(m_calendarEventEditAction, QStringLiteral("calendar_event_edit"),
                               QStringLiteral("document-edit"), i18n("Edit Event…"));
        addCalendarEventAction(m_calendarEventDuplicateAction,
                               QStringLiteral("calendar_event_duplicate"),
                               QStringLiteral("edit-copy"), i18n("Duplicate Event…"));
        addCalendarEventAction(m_calendarEventMoveAction, QStringLiteral("calendar_event_move"),
                               QStringLiteral("mail-move"), i18n("Move to Calendar"));
        addCalendarEventAction(m_calendarEventAcceptAction, QStringLiteral("calendar_event_accept"),
                               QStringLiteral("dialog-ok-apply"),
                               i18nc("@action calendar RSVP", "Accept"));
        addCalendarEventAction(
            m_calendarEventTentativeAction, QStringLiteral("calendar_event_tentative"),
            QStringLiteral("dialog-question"), i18nc("@action calendar RSVP", "Tentative"));
        addCalendarEventAction(
            m_calendarEventDeclineAction, QStringLiteral("calendar_event_decline"),
            QStringLiteral("dialog-cancel"), i18nc("@action calendar RSVP", "Decline"));
        addCalendarEventAction(m_calendarEventCopyDetailsAction,
                               QStringLiteral("calendar_event_copy_details"),
                               QStringLiteral("edit-copy"), i18n("Copy Event Details"));
        addCalendarEventAction(m_calendarEventDeleteAction, QStringLiteral("calendar_event_delete"),
                               QStringLiteral("edit-delete"), i18n("Delete Event…"));

        auto* logAction = new QAction(QIcon::fromTheme(QStringLiteral("view-list-text")),
                                      i18n("Application Log"), this);
        connect(logAction, &QAction::triggered, this,
                [this]
                {
                    auto* dialog =
                        new javelin::gui::logging::LogViewerDialog(m_daemonLogPort, this);
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                });
        actionCollection()->addAction(QStringLiteral("open_application_log"), logAction);

        m_mailActionController = new MailActionController(
            m_mailboxReader, m_mailTagReader, m_mailCommandPort, *m_messageSelectionController,
            *m_messageCommandController, *m_quickFilterController, *m_messageView, *m_tagsMenu,
            *this,
            {.newMessage = *m_newMessageAction,
             .reply = *m_replyAction,
             .replyAll = *m_replyAllAction,
             .forward = *m_forwardAction,
             .editDraft = *m_editDraftAction,
             .archive = *m_archiveAction,
             .markUnread = *m_markUnreadAction,
             .star = *m_starAction,
             .junk = *m_junkAction,
             .tags = *m_tagsAction,
             .deleteFromMailbox = *m_deleteAction,
             .permanentDelete = *m_permanentDeleteAction,
             .move = *m_moveAction,
             .copy = *m_copyAction,
             .save = *m_saveCurrentAction,
             .viewSource = *m_viewSourceAction,
             .findSender = *m_findSenderContextAction},
            [this](QString message, const int durationMilliseconds)
            { m_statusBar->showMessage(message, durationMilliseconds); },
            [this](const javelin::jmap::OperationError& error) { presentError(error); },
            [this] { refreshMessageListPreservingSelection(); }, this);
        m_mailActionController->activate(activeTab());

        m_themeController->setThemedActions({
            {m_refreshAction, QStringLiteral(":/icons/thunderbird-icons/cloud-download.svg")},
            {m_preferencesAction, QStringLiteral(":/icons/thunderbird-icons/settings.svg")},
            {m_newMessageAction, QStringLiteral(":/icons/thunderbird-icons/new-mail.svg")},
            {m_replyAction, QStringLiteral(":/icons/thunderbird-icons/reply.svg")},
            {m_replyAllAction, QStringLiteral(":/icons/thunderbird-icons/reply-all.svg")},
            {m_forwardAction, QStringLiteral(":/icons/thunderbird-icons/forward.svg")},
            {m_editDraftAction, QStringLiteral(":/icons/thunderbird-icons/draft.svg")},
            {m_archiveAction, QStringLiteral(":/icons/thunderbird-icons/archive.svg")},
            {m_markUnreadAction, QStringLiteral(":/icons/thunderbird-icons/unread.svg")},
            {m_starAction, QStringLiteral(":/icons/thunderbird-icons/star.svg")},
            {m_junkAction, QStringLiteral(":/icons/thunderbird-icons/spam.svg")},
            {m_tagsAction, QStringLiteral(":/icons/thunderbird-icons/tag.svg")},
            {m_deleteAction, QStringLiteral(":/icons/thunderbird-icons/delete.svg")},
            {m_advancedSearchAction, QStringLiteral(":/icons/thunderbird-icons/search.svg")},
        });
    }

    void MainWindow::routeUndo()
    {
        if (FocusedCommandRouter::invokeNativeCommand(QApplication::focusWidget(),
                                                      EditHistoryDirection::Undo))
        {
            updateUndoRedoActions();
            return;
        }
        auto task = m_undoCommandPort.undo();
        QCoro::connect(std::move(task), this, [](const bool) {});
    }

    void MainWindow::routeRedo()
    {
        if (FocusedCommandRouter::invokeNativeCommand(QApplication::focusWidget(),
                                                      EditHistoryDirection::Redo))
        {
            updateUndoRedoActions();
            return;
        }
        auto task = m_undoCommandPort.redo();
        QCoro::connect(std::move(task), this, [](const bool) {});
    }

    void MainWindow::routeSaveCurrent()
    {
        if (activeTabIsCompose())
        {
            m_composeTabController->saveDraft(activeTab());
            return;
        }
        if (activeTabIsContacts())
        {
            m_contactsTabController->invoke(activeTab(), ContactsTabCommand::ExportVCard);
            return;
        }
        if (activeTabIsMailbox() || activeTabIsSearch())
        {
            saveSelectedMessages();
            return;
        }
        m_statusBar->showMessage(i18n("There is nothing to save in this view."), 3000);
    }

    void MainWindow::saveSelectedMessages()
    {
        const auto accountId = activeAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a message to save."), 3000);
            return;
        }
        auto selection = m_messageCommandController->selectedActionItems();
        if (selection.empty())
        {
            m_statusBar->showMessage(i18n("Select a message to save."), 3000);
            return;
        }
        // Save follows conversation semantics rather than mailbox-mutation semantics: a collapsed
        // Thread represents the whole conversation, including members currently filed elsewhere.
        m_messageFileController->saveMessages(*accountId, std::nullopt, std::move(selection));
    }

    void MainWindow::exportCurrentMailbox()
    {
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            m_statusBar->showMessage(i18n("Open a mailbox to export it."), 3000);
            return;
        }
        m_mailExportController->exportMailbox(*accountId, *mailboxId);
    }

    void MainWindow::exportCurrentAccount()
    {
        const auto accountId = preferredMailAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a mail account to export it."), 3000);
            return;
        }
        m_mailExportController->exportAccount(*accountId);
    }

    void MainWindow::updateUndoRedoActions()
    {
        if (m_undoAction == nullptr || m_redoAction == nullptr)
            return;
        auto* focus = QApplication::focusWidget();
        const bool nativeUndo =
            FocusedCommandRouter::isNativeCommandAvailable(focus, EditHistoryDirection::Undo);
        const bool nativeRedo =
            FocusedCommandRouter::isNativeCommandAvailable(focus, EditHistoryDirection::Redo);
        const auto& state = m_undoCommandPort.state();
        m_undoAction->setText(nativeUndo ? i18nc("@action", "Undo") : state.undoLabel);
        m_redoAction->setText(nativeRedo ? i18nc("@action", "Redo") : state.redoLabel);
        m_undoAction->setEnabled(nativeUndo || state.canUndo);
        m_redoAction->setEnabled(nativeRedo || state.canRedo);
    }

    void MainWindow::presentHistoryFailure(const javelin::app::undo::HistoryFailure& failure)
    {
        QString details;
        for (const auto& objectFailure : failure.objectFailures)
        {
            if (!details.isEmpty())
                details += QLatin1Char('\n');
            details += objectFailure.objectId + QStringLiteral(": ") + objectFailure.summary;
        }

        QMessageBox messageBox{QMessageBox::Critical, failure.actionLabel, failure.summary,
                               QMessageBox::Ok, this};
        if (!details.isEmpty())
            messageBox.setDetailedText(details);
        QAbstractButton* removeButton = nullptr;
        if (failure.mayRemoveFromHistory && !failure.acknowledgeAndRemove)
        {
            removeButton =
                messageBox.addButton(i18n("Remove from History"), QMessageBox::DestructiveRole);
        }
        messageBox.exec();
        if (failure.acknowledgeAndRemove)
        {
            static_cast<void>(m_undoCommandPort.acknowledgeAndRemove(failure.entryId));
        }
        else if (removeButton != nullptr && messageBox.clickedButton() == removeButton)
        {
            static_cast<void>(m_undoCommandPort.forget(failure.entryId));
        }
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(i18n("Javelin Mail"));

        m_mailboxModel = new javelin::gui::mailboxes::MailboxTreeModel(
            m_accountReader, m_mailboxReader, m_databasePath,
            {.accountId = std::nullopt,
             .showAccount = true,
             .checkable = false,
             .checkedMailboxIds = {},
             .accountDisplayName = [this](const QStringView accountId)
             { return m_settings.accountForCachedId(accountId).displayName; }},
            this);
        m_messageModel = new javelin::gui::messages::MessageListModel(m_databasePath, this);
        connect(m_messageModel,
                &javelin::gui::messages::MessageListModel::threadMaterializationRequired, this,
                [this](const QString& threadId)
                {
                    auto* tab = activeTab();
                    auto* session = tab == nullptr ? nullptr : messageListSession(*tab);
                    if (session != nullptr)
                        session->ensureThreadMaterialized(threadId.toStdString());
                });

        m_mailboxSearchEdit = new QLineEdit(this);
        m_mailboxSearchEdit->setClearButtonEnabled(true);
        m_mailboxSearchEdit->setPlaceholderText(i18n("Search this account on the server"));

        m_tabBar = new QTabBar(this);
        m_tabBar->setDocumentMode(true);
        m_tabBar->setTabsClosable(true);
        m_tabBar->setMovable(true);
        m_tabBar->setExpanding(false);
        m_tabBar->setElideMode(Qt::ElideRight);
        m_tabBar->setUsesScrollButtons(true);
        m_tabBar->setStyleSheet(
            QStringLiteral("QTabBar::tab { max-width: 220px; min-width: 120px; }"));
        m_tabBar->hide();
        m_tabBarPresenter = new TabBarPresenter(m_settings, *m_tabBar, *this, this);

        m_mailboxView = new javelin::gui::mailboxes::MailboxTreeView(this);
        m_mailboxView->setModel(m_mailboxModel);
        connect(m_mailboxModel, &QAbstractItemModel::modelReset, m_mailboxView,
                &QTreeView::expandAll);
        connect(m_mailboxModel, &QAbstractItemModel::modelReset, this,
                [this] { selectPendingInitialMailbox(); });
        m_mailboxView->expandAll();
        m_mailboxView->setContextMenuPolicy(Qt::CustomContextMenu);
        m_mailboxView->setAcceptDrops(true);
        m_mailboxView->setDropIndicatorShown(true);
        m_mailboxView->setDragDropMode(QAbstractItemView::DropOnly);

        m_mailboxPane = new QWidget(this);
        auto* mailboxLayout = new QVBoxLayout(m_mailboxPane);
        mailboxLayout->setContentsMargins(0, 0, 0, 0);
        mailboxLayout->setSpacing(6);
        mailboxLayout->addWidget(m_mailboxSearchEdit);
        mailboxLayout->addWidget(m_mailboxView);

        m_messageView = new javelin::gui::messages::MessageDragListView(this);
        m_messageView->setModel(m_messageModel);
        auto* messageListDelegate = new javelin::gui::messages::MessageListDelegate(m_messageView);
        m_messageView->setItemDelegate(messageListDelegate);
        m_messageView->setSpacing(6);
        m_messageView->setFrameShape(QFrame::NoFrame);
        m_messageView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_messageView->setSelectionMode(QAbstractItemView::ExtendedSelection);
        m_messageView->setDragEnabled(true);
        m_messageView->setDragDropMode(QAbstractItemView::DragOnly);
        m_messageView->setDefaultDropAction(Qt::MoveAction);
        m_messageView->setStyleSheet(QStringLiteral("QListView { border: none; padding: 3px; }"));
        m_messageView->setMouseTracking(true);
        m_messageView->viewport()->setMouseTracking(true);
        m_messageView->installEventFilter(this);
        m_messageView->viewport()->installEventFilter(this);

        m_messageCommandController = new MessageCommandController(
            m_mailCommandPort, m_accountReader, m_mailboxReader, [this](const QStringView accountId)
            { return m_settings.accountForCachedId(accountId).displayName; }, *m_messageView, this,
            this);
        connect(m_messageCommandController, &MessageCommandController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_messageCommandController, &MessageCommandController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(m_messageCommandController, &MessageCommandController::mailboxMembershipChanged,
                this,
                [this](const QString&)
                {
                    refreshMessageListPreservingSelection();
                    m_messageModel->refreshExpandedThreadMembers();
                    refreshSelectionFromModels();
                    updateEmptyStates();
                    updateMessageListHeader();
                });
        connect(m_messageCommandController, &MessageCommandController::messageMetadataChanged, this,
                [this](const QString& accountId)
                {
                    markSearchTabsStaleForAccount(accountId.toStdString());
                    refreshMessageListPreservingSelection();
                    m_messageModel->refreshExpandedThreadMembers();
                    refreshSelectionFromModels();
                });
        connect(m_messageCommandController, &MessageCommandController::junkStateChanged, this,
                [this](const QString& accountId)
                {
                    markSearchTabsStaleForAccount(accountId.toStdString());
                    refreshMessageListPreservingSelection();
                    m_messageModel->refreshExpandedThreadMembers();
                    refreshSelectionFromModels();
                    m_messageViewContainer->refresh(m_messageViewReader);
                    updateEmptyStates();
                    updateMessageListHeader();
                    m_mailActionController->update();
                });
        connect(m_messageCommandController, &MessageCommandController::emailMarkedRead, this,
                [this](const QString& accountId, const QString& emailId)
                {
                    markSearchTabsStaleForAccount(accountId.toStdString());
                    static_cast<void>(m_messageModel->setEmailRead(emailId.toStdString()));
                    m_mailActionController->update();
                });
        connect(
            m_messageCommandController, &MessageCommandController::emailMutationsSubmitted, this,
            [this](const EmailMutationSubmissionSummary& summary)
            {
                if (summary.failedEmailCount == 0)
                    return;
                presentError(javelin::jmap::OperationError{
                    .message = i18np("The server rejected %1 email mutation. The confirmed mailbox "
                                     "state has been restored.",
                                     "The server rejected %1 email mutations. The confirmed "
                                     "mailbox state has been restored.",
                                     summary.failedEmailCount),
                });
            });

        connect(m_mailboxModel, &javelin::gui::mailboxes::MailboxTreeModel::emailsDropped, this,
                [this](const javelin::gui::messages::MessageDragPayload& payload,
                       const QString& destinationAccountId, const QString& destinationMailboxId,
                       const Qt::DropAction action)
                {
                    if (action != Qt::MoveAction && action != Qt::CopyAction)
                        return;
                    const bool copy = action == Qt::CopyAction;
                    m_messageCommandController->queueTransfer(
                        payload.sourceAccountId, payload.sourceMailboxId,
                        destinationAccountId.toStdString(), destinationMailboxId.toStdString(),
                        payload.selection,
                        copy ? MessageTransferOperation::Copy : MessageTransferOperation::Move,
                        copy ? i18n("Queued copy.") : i18n("Queued move."));
                });

        auto* messagePane = new QWidget(this);
        auto* messageLayout = new QVBoxLayout(messagePane);
        messageLayout->setContentsMargins(0, 0, 0, 0);
        messageLayout->setSpacing(0);
        auto* messageHeader = new QWidget(messagePane);
        auto* messageHeaderLayout = new QVBoxLayout(messageHeader);
        messageHeaderLayout->setContentsMargins(0, 0, 0, 0);
        messageHeaderLayout->setSpacing(0);
        auto* messageHeaderRow = new QWidget(messageHeader);
        auto* messageHeaderRowLayout = new QHBoxLayout(messageHeaderRow);
        messageHeaderRowLayout->setContentsMargins(8, 3, 8, 3);
        messageHeaderRowLayout->setSpacing(8);
        m_messageListTitleLabel = new ElidingLabel(messageHeaderRow);
        m_messageListMetaLabel = new QLabel(messageHeaderRow);
        m_searchServerButton = new QToolButton(messageHeaderRow);
        m_searchServerButton->setText(i18n("Search server"));
        m_searchServerButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_searchServerButton->setToolTip(
            i18n("Replace results indexed on this device with authoritative server results"));
        m_searchServerButton->setVisible(false);
        const auto quickFilterIcon = [this](const QString& resourcePath)
        {
            return javelin::gui::themedSvgIcon(resourcePath,
                                               palette().color(QPalette::Active, QPalette::Text));
        };
        auto* quickFilterButton = new QToolButton(messageHeaderRow);
        quickFilterButton->setText(i18n("Quick Filter"));
        quickFilterButton->setIcon(
            quickFilterIcon(QStringLiteral(":/icons/thunderbird-icons/filter.svg")));
        quickFilterButton->setCheckable(true);
        quickFilterButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        quickFilterButton->setToolTip(i18n("Show quick filter controls"));
        m_messageSortButton = new QToolButton(messageHeaderRow);
        m_messageSortButton->setIcon(javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/display-options.svg"),
            palette().color(QPalette::Text)));
        m_messageSortButton->setAccessibleName(i18n("Sort messages"));
        m_messageSortButton->setToolTip(i18n("Sort messages"));
        auto titleFont = javelin::gui::fontWithSizeDelta(m_messageListTitleLabel->font(), 4);
        titleFont.setBold(true);
        m_messageListTitleLabel->setFont(titleFont);
        messageHeaderRowLayout->addWidget(m_messageListTitleLabel, 1);
        messageHeaderRowLayout->addWidget(m_messageListMetaLabel);
        messageHeaderRowLayout->addWidget(m_searchServerButton);
        messageHeaderRowLayout->addWidget(quickFilterButton);
        messageHeaderRowLayout->addWidget(m_messageSortButton);
        messageHeaderLayout->addWidget(messageHeaderRow);

        auto* quickFilterPanel = new QWidget(messageHeader);
        auto* quickFilterLayout = new QVBoxLayout(quickFilterPanel);
        quickFilterLayout->setContentsMargins(8, 3, 8, 3);
        quickFilterLayout->setSpacing(3);
        auto* quickFilterButtonsRow = new QWidget(quickFilterPanel);
        auto* quickFilterButtonsLayout = new QHBoxLayout(quickFilterButtonsRow);
        quickFilterButtonsLayout->setContentsMargins(0, 0, 0, 0);
        quickFilterButtonsLayout->setSpacing(4);

        const auto makeFilterButton =
            [quickFilterButtonsRow, &quickFilterIcon](const QString& text, const QString& iconPath)
        {
            auto* button = new QToolButton(quickFilterButtonsRow);
            button->setText(text);
            button->setIcon(quickFilterIcon(iconPath));
            button->setCheckable(true);
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            return button;
        };
        auto* quickFilterPinButton =
            makeFilterButton(i18n("Pin"), QStringLiteral(":/icons/thunderbird-icons/pin.svg"));
        quickFilterPinButton->setToolTip(i18n("Keep these filters when switching folders"));
        auto* quickFilterUnreadButton = makeFilterButton(
            i18n("Unread"), QStringLiteral(":/icons/thunderbird-icons/unread.svg"));
        auto* quickFilterStarredButton =
            makeFilterButton(i18n("Starred"), QStringLiteral(":/icons/thunderbird-icons/star.svg"));
        auto* quickFilterContactButton = makeFilterButton(
            i18n("Contact"), QStringLiteral(":/icons/thunderbird-icons/address-book.svg"));
        auto* quickFilterTagsButton =
            makeFilterButton(i18n("Tags"), QStringLiteral(":/icons/thunderbird-icons/tag.svg"));
        auto* quickFilterAttachmentButton = makeFilterButton(
            i18n("Attachment"), QStringLiteral(":/icons/thunderbird-icons/attachment.svg"));
        auto* quickFilterTagsMenu = new QMenu(quickFilterTagsButton);
        quickFilterTagsButton->setMenu(quickFilterTagsMenu);
        quickFilterTagsButton->setPopupMode(QToolButton::MenuButtonPopup);

        quickFilterButtonsLayout->addWidget(quickFilterPinButton);
        quickFilterButtonsLayout->addWidget(quickFilterUnreadButton);
        quickFilterButtonsLayout->addWidget(quickFilterStarredButton);
        quickFilterButtonsLayout->addWidget(quickFilterContactButton);
        quickFilterButtonsLayout->addWidget(quickFilterTagsButton);
        quickFilterButtonsLayout->addWidget(quickFilterAttachmentButton);
        quickFilterButtonsLayout->addStretch(1);
        quickFilterLayout->addWidget(quickFilterButtonsRow);

        auto* quickFilterTextRow = new QWidget(quickFilterPanel);
        auto* quickFilterTextLayout = new QHBoxLayout(quickFilterTextRow);
        quickFilterTextLayout->setContentsMargins(0, 0, 0, 0);
        quickFilterTextLayout->setSpacing(4);
        auto* quickFilterTextEdit = new QLineEdit(quickFilterTextRow);
        quickFilterTextEdit->setPlaceholderText(i18n("Filter messages"));
        quickFilterTextEdit->setClearButtonEnabled(true);
        const auto makeScopeButton = [quickFilterTextRow](const QString& text, const bool checked)
        {
            auto* button = new QToolButton(quickFilterTextRow);
            button->setText(text);
            button->setCheckable(true);
            button->setChecked(checked);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            return button;
        };
        auto* quickFilterSenderButton = makeScopeButton(i18n("Sender"), true);
        auto* quickFilterRecipientsButton = makeScopeButton(i18n("Recipients"), true);
        auto* quickFilterSubjectButton = makeScopeButton(i18n("Subject"), true);
        auto* quickFilterBodyButton = makeScopeButton(i18n("Body"), false);
        quickFilterTextLayout->addWidget(quickFilterTextEdit, 1);
        quickFilterTextLayout->addWidget(quickFilterSenderButton);
        quickFilterTextLayout->addWidget(quickFilterRecipientsButton);
        quickFilterTextLayout->addWidget(quickFilterSubjectButton);
        quickFilterTextLayout->addWidget(quickFilterBodyButton);
        quickFilterLayout->addWidget(quickFilterTextRow);
        quickFilterPanel->setVisible(false);
        messageHeaderLayout->addWidget(quickFilterPanel);
        m_quickFilterController =
            new QuickFilterController(m_mailTagReader,
                                      {.toggleButton = *quickFilterButton,
                                       .panel = *quickFilterPanel,
                                       .pinButton = *quickFilterPinButton,
                                       .unreadButton = *quickFilterUnreadButton,
                                       .starredButton = *quickFilterStarredButton,
                                       .contactButton = *quickFilterContactButton,
                                       .tagsButton = *quickFilterTagsButton,
                                       .attachmentButton = *quickFilterAttachmentButton,
                                       .textEdit = *quickFilterTextEdit,
                                       .senderButton = *quickFilterSenderButton,
                                       .recipientsButton = *quickFilterRecipientsButton,
                                       .subjectButton = *quickFilterSubjectButton,
                                       .bodyButton = *quickFilterBodyButton,
                                       .tagsMenu = *quickFilterTagsMenu},
                                      *this, this);
        m_messageLoadingIndicator =
            new javelin::gui::widgets::IndeterminateProgressBar(messageHeader);
        m_messageLoadingIndicator->setAccessibleName(i18n("Loading messages"));
        m_messageLoadingIndicator->setFixedHeight(2);
        m_messageLoadingIndicator->setTextVisible(false);
        m_messageLoadingIndicator->setRange(0, 1);
        m_messageLoadingIndicator->setValue(0);
        m_messageLoadingIndicator->setStyleSheet(
            QStringLiteral("QProgressBar { border: none; background: transparent; }"
                           "QProgressBar::chunk { background-color: palette(highlight); }"));
        messageHeaderLayout->addWidget(m_messageLoadingIndicator);

        auto* messageListArea = new QWidget(messagePane);
        auto* messageListAreaLayout = new QGridLayout(messageListArea);
        messageListAreaLayout->setContentsMargins(0, 0, 0, 0);
        messageListAreaLayout->setSpacing(0);
        m_messageEmptyStatePanel = new QWidget(messageListArea);
        m_messageEmptyStatePanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        auto* messageEmptyStateLayout = new QVBoxLayout(m_messageEmptyStatePanel);
        messageEmptyStateLayout->setContentsMargins(24, 24, 24, 24);
        messageEmptyStateLayout->setSpacing(10);
        messageEmptyStateLayout->addStretch(1);
        m_messageEmptyState = new QLabel(i18n("This mailbox is empty."), m_messageEmptyStatePanel);
        m_messageEmptyState->setAlignment(Qt::AlignCenter);
        m_messageEmptyState->setWordWrap(true);
        m_messageEmptyState->setTextInteractionFlags(Qt::TextSelectableByMouse);
        messageEmptyStateLayout->addWidget(m_messageEmptyState);
        m_messageEmptyStateActionButton = new QToolButton(m_messageEmptyStatePanel);
        m_messageEmptyStateActionButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_messageEmptyStateActionButton->setVisible(false);
        messageEmptyStateLayout->addWidget(m_messageEmptyStateActionButton, 0, Qt::AlignHCenter);
        messageEmptyStateLayout->addStretch(1);
        messageListAreaLayout->addWidget(m_messageView, 0, 0);
        messageListAreaLayout->addWidget(m_messageEmptyStatePanel, 0, 0);
        m_messageListFooter = new QWidget(messagePane);
        auto* messageListFooterLayout = new QHBoxLayout(m_messageListFooter);
        messageListFooterLayout->setContentsMargins(8, 3, 8, 3);
        messageListFooterLayout->setSpacing(8);
        m_messageListFooterLabel = new QLabel(m_messageListFooter);
        m_messageListFooterRetryButton = new QToolButton(m_messageListFooter);
        m_messageListFooterRetryButton->setText(i18n("Retry"));
        m_messageListFooterRetryButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        messageListFooterLayout->addWidget(m_messageListFooterLabel, 1);
        messageListFooterLayout->addWidget(m_messageListFooterRetryButton);
        m_messageListFooter->setVisible(false);
        messageLayout->addWidget(messageHeader);
        messageLayout->addWidget(messageListArea, 1);
        messageLayout->addWidget(m_messageListFooter);

        m_messageViewContainer = new javelin::gui::messageview::MessageViewContainer(
            m_settings, m_translationService, m_contactIdentityLookup, this);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::saveAttachmentRequested, this,
                [this](const QString& accountId, const QString& emailId, const QString& partId)
                {
                    m_messageFileController->saveAttachment(
                        accountId.toStdString(), emailId.toStdString(), partId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::saveAllAttachmentsRequested, this,
                [this](const QString& accountId, const QString& emailId)
                {
                    m_messageFileController->saveAllAttachments(accountId.toStdString(),
                                                                emailId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::openAttachmentRequested, this,
                [this](const QString& accountId, const QString& emailId, const QString& partId)
                {
                    m_messageFileController->openAttachment(
                        accountId.toStdString(), emailId.toStdString(), partId.toStdString());
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::viewSourceRequested, this,
                &MainWindow::viewSelectedMessageSource);
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::notJunkRequested, this,
                [this](const QString& accountId, const QString& mailboxId, const QString& emailId)
                {
                    m_messageCommandController->setEmailJunk(
                        accountId.toStdString(),
                        mailboxId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                            : std::optional<std::string>{mailboxId.toStdString()},
                        emailId.toStdString(), false);
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::messageActivated, this,
                [this](const QString& emailId)
                {
                    if (m_messageSelectionController->selectMessageAlone(emailId))
                        refreshSelectionFromModels();
                });
        connect(m_messageViewContainer,
                &javelin::gui::messageview::MessageViewContainer::hoveredLinkChanged, this,
                [this](const QString& url) { m_statusBar->setOverlayMessage(url); });

        m_messageView->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(m_messageView, &QListView::customContextMenuRequested, this,
                [this](const QPoint& position)
                {
                    m_mailActionController->showContextMenu(
                        position,
                        [this](QModelIndex index) { findConversationsWithSender(index); });
                });
        connect(m_mailboxView, &QTreeView::customContextMenuRequested, this,
                &MainWindow::showMailboxContextMenu);
        connect(
            messageListDelegate,
            &javelin::gui::messages::MessageListDelegate::threadExpansionToggled, this,
            [this](const QModelIndex& index)
            {
                if (!index.isValid())
                {
                    return;
                }

                const auto threadId =
                    index.data(javelin::gui::messages::MessageListModel::ThreadIdRole).toString();
                if (threadId.isEmpty())
                {
                    return;
                }

                const bool isExpanded =
                    index.data(javelin::gui::messages::MessageListModel::IsExpandedRole).toBool();
                if (isExpanded && m_messageSelectionController->currentThreadId() ==
                                      std::optional<std::string>{threadId.toStdString()})
                {
                    m_messageView->setCurrentIndex(index);
                }

                static_cast<void>(
                    m_messageModel->setThreadExpanded(threadId.toStdString(), !isExpanded));
            });
        connect(messageListDelegate,
                &javelin::gui::messages::MessageListDelegate::attachmentButtonClicked, this,
                [this](const QModelIndex& index)
                {
                    if (!index.isValid())
                    {
                        return;
                    }
                    m_messageView->setCurrentIndex(index);
                    refreshSelectionFromModels();
                });
        connect(messageListDelegate, &javelin::gui::messages::MessageListDelegate::flaggedToggled,
                this, [this](const QModelIndex& index)
                { m_messageCommandController->toggleFlagged(activeAccountId(), index); });

        m_mainSplitter = new QSplitter(Qt::Horizontal, this);
        m_mainSplitter->addWidget(m_mailboxPane);
        m_mainSplitter->addWidget(messagePane);
        m_mainSplitter->addWidget(m_messageViewContainer);
        m_mainSplitter->setStretchFactor(0, 1);
        m_mainSplitter->setStretchFactor(1, 2);
        m_mainSplitter->setStretchFactor(2, 3);
        m_mainSplitter->setSizes({240, 420, 780});

        auto* centralContainer = new QWidget(this);
        auto* centralLayout = new QVBoxLayout(centralContainer);
        centralLayout->setContentsMargins(0, 0, 0, 0);
        centralLayout->setSpacing(0);
        m_contentStack = new QStackedWidget(centralContainer);
        m_contentStack->addWidget(m_mainSplitter);
        centralLayout->addWidget(m_tabBar);
        centralLayout->addWidget(m_contentStack);

        setCentralWidget(centralContainer);

        QWidget::setTabOrder(m_mailboxView, m_searchServerButton);
        QWidget::setTabOrder(m_searchServerButton, quickFilterButton);
        QWidget::setTabOrder(quickFilterButton, quickFilterPinButton);
        QWidget::setTabOrder(quickFilterPinButton, quickFilterUnreadButton);
        QWidget::setTabOrder(quickFilterUnreadButton, quickFilterStarredButton);
        QWidget::setTabOrder(quickFilterStarredButton, quickFilterContactButton);
        QWidget::setTabOrder(quickFilterContactButton, quickFilterTagsButton);
        QWidget::setTabOrder(quickFilterTagsButton, quickFilterAttachmentButton);
        QWidget::setTabOrder(quickFilterAttachmentButton, quickFilterTextEdit);
        QWidget::setTabOrder(quickFilterTextEdit, quickFilterSenderButton);
        QWidget::setTabOrder(quickFilterSenderButton, quickFilterRecipientsButton);
        QWidget::setTabOrder(quickFilterRecipientsButton, quickFilterSubjectButton);
        QWidget::setTabOrder(quickFilterSubjectButton, quickFilterBodyButton);
        QWidget::setTabOrder(quickFilterBodyButton, m_messageSortButton);
        QWidget::setTabOrder(m_messageSortButton, m_messageView);
        QWidget::setTabOrder(m_messageView, m_messageViewContainer);

        m_messageListPanePresenter =
            std::make_unique<javelin::gui::messages::MessageListPanePresenter>(
                *m_messageListTitleLabel, *m_messageListMetaLabel, *m_messageEmptyStatePanel,
                *m_messageEmptyState, *m_messageEmptyStateActionButton, *m_messageView,
                *m_messageLoadingIndicator, *m_searchServerButton, *m_messageListFooter,
                *m_messageListFooterLabel, *m_messageListFooterRetryButton);
        m_messageListTabPresenter = std::make_unique<MessageListTabPresenter>(
            *m_messageListPanePresenter, *m_tabBarPresenter);
        updateEmptyStates();
        updateMessageListHeader();
        m_quickFilterController->activate(activeTab());
    }

    void MainWindow::connectSelection()
    {
        connect(m_tabBar, &QTabBar::currentChanged, this,
                [this](const int index)
                {
                    m_messageNavigationPort.cancel();
                    activateTab(index, true);
                });
        connect(m_tabBar, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
        connect(m_tabBar, &QTabBar::tabMoved, this,
                [this](const int fromIndex, const int toIndex)
                {
                    if (!m_mailWorkspaceController->moveTab(fromIndex, toIndex))
                    {
                        const QSignalBlocker blocker{m_tabBar};
                        m_tabBar->moveTab(toIndex, fromIndex);
                        return;
                    }
                    updateTabBar();
                    if (m_activeTabIndex.has_value())
                        activateTab(*m_activeTabIndex, false);
                });
        connect(m_tabBarPresenter, &TabBarPresenter::closeRequested, this, &MainWindow::closeTab);
        connect(m_messageListFooterRetryButton, &QToolButton::clicked, this,
                &MainWindow::loadMoreMessages);
        connect(m_messageEmptyStateActionButton, &QToolButton::clicked, this,
                &MainWindow::activateMessageListEmptyAction);
        connect(m_messageView->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { maybeLoadMoreMessages(); });
        connect(m_messageView->verticalScrollBar(), &QScrollBar::rangeChanged, this,
                [this] { QTimer::singleShot(0, this, &MainWindow::maybeLoadMoreMessages); });
        connect(m_messageSortButton, &QToolButton::clicked, this, &MainWindow::showSortMenu);
        connect(m_searchServerButton, &QToolButton::clicked, this,
                [this]
                {
                    if (auto* tab = activeTab(); tab != nullptr)
                        static_cast<void>(m_mailWorkspaceController->promoteSearch(*tab));
                });
        connect(m_mailboxSearchEdit, &QLineEdit::returnPressed, this,
                [this] { executeSearch(m_mailboxSearchEdit->text()); });
        connect(m_mailboxSearchEdit, &QLineEdit::textChanged, this,
                [this](const QString& text)
                {
                    if (text.trimmed().isEmpty())
                    {
                        clearSearch();
                    }
                });

        connect(m_mailboxView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    if (current.isValid())
                        activateMailboxSelection(false);
                });
        connect(m_mailboxView, &QTreeView::doubleClicked, this,
                [this](const QModelIndex& index)
                {
                    if (!index.isValid())
                    {
                        return;
                    }

                    openMailboxSelectionInTab(false);
                });

        connect(m_messageView->selectionModel(), &QItemSelectionModel::currentChanged, this,
                [this](const QModelIndex& current, const QModelIndex&)
                {
                    handleCurrentMessageChanged(current);
                    updateActiveContextUi();
                });
        connect(m_messageView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this](const QItemSelection&, const QItemSelection&)
                { refreshSelectionFromModels(); });
    }

    void MainWindow::handleCurrentMessageChanged(const QModelIndex& current)
    {
        if (m_modelUpdateInProgress)
            return;
        auto* tab = activeTab();
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        const bool allowSearchSelection = activeTabIsSearch() && accountId.has_value();
        if (!accountId.has_value() || (!mailboxId.has_value() && !allowSearchSelection))
        {
            setMessageViewSelection(accountId, mailboxId, std::nullopt);
            m_mailActionController->update();
            return;
        }

        if (!current.isValid())
        {
            m_quickFilterController->syncContinuitySelection(std::nullopt, std::nullopt);
            setMessageViewSelection(accountId, mailboxId, std::nullopt);
            m_mailActionController->update();
            return;
        }

        const auto emailId =
            current.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
        const auto threadId =
            current.data(javelin::gui::messages::MessageListModel::ThreadIdRole).toString();
        const auto selectedEmailId = emailId.toStdString();
        const auto selectedThreadId = threadId.toStdString();
        m_quickFilterController->syncContinuitySelection(
            emailId.isEmpty() ? std::nullopt : std::optional<std::string>{selectedEmailId},
            threadId.isEmpty() ? std::nullopt : std::optional<std::string>{selectedThreadId});
        m_messageNavigationController->cancelIfSelectionChanged(
            tab, selectedEmailId, std::optional<std::string_view>{selectedThreadId});
        const bool isUnread = indexIsUnread(current);
        if (!threadId.isEmpty() && !activeTabIsSearch())
        {
            QTimer::singleShot(0, this,
                               [this, threadId = threadId.toStdString()]
                               {
                                   if (m_messageSelectionController->currentThreadId() ==
                                       std::optional<std::string>{threadId})
                                   {
                                       static_cast<void>(
                                           m_messageModel->setThreadExpanded(threadId, true));
                                   }
                               });
        }
        setMessageViewSelection(accountId, mailboxId,
                                emailId.isEmpty()
                                    ? std::optional<std::string>{std::nullopt}
                                    : std::optional<std::string>{emailId.toStdString()});
        updateEmptyStates();
        updateMessageListHeader();
        m_mailActionController->update();
        if (!emailId.isEmpty())
        {
            if (isUnread)
            {
                m_messageCommandController->markEmailRead(*accountId, emailId.toStdString());
            }
        }

        m_messageSelectionController->syncTabSelection(activeTab());
    }

    void MainWindow::openContacts()
    {
        m_contactsTabController->open();
    }

    void MainWindow::openSieveEditor()
    {
        const auto accountId = preferredMailAccountId();
        if (!accountId)
        {
            m_statusBar->showMessage(i18n("Select an account to edit its Sieve rules."), 5000);
            return;
        }
        javelin::gui::sieve::SieveEditorDialog dialog{m_sieveCommandPort, *accountId, this};
        dialog.exec();
    }

    void MainWindow::openSendingIdentities()
    {
        openSendingIdentitiesFor({}, {});
    }

    void MainWindow::openSendingIdentitiesFor(QString accountId, QString identityId)
    {
        javelin::gui::identity::IdentityManagerDialog dialog{
            m_settings, m_accountReader, m_identityReader, m_identityCommandPort, m_mailEvents,
            this};
        if (!accountId.isEmpty() && !identityId.isEmpty())
            dialog.selectIdentity(accountId.toStdString(), identityId.toStdString());
        dialog.exec();
    }

    void MainWindow::openCalendar()
    {
        m_calendarTabController->open();
    }

    void MainWindow::openCalendarEvent(const QString& calendarAccountId, const QString& eventId,
                                       const QString& recurrenceId, const QDate& navigationDate)
    {
        m_calendarTabController->openEvent(calendarAccountId, eventId, recurrenceId,
                                           navigationDate);
    }

    void MainWindow::composeNewMessage()
    {
        const auto accountId = preferredSubmissionAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("No account is available for sending mail."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .referenceEmailId = std::nullopt,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::openMailtoUri(const QString& uri)
    {
        const auto parsed = javelin::gui::compose::parseMailtoUri(uri);
        if (!parsed.has_value())
        {
            m_statusBar->showMessage(i18n("The mail link is invalid."), 7000);
            return;
        }

        const auto accountId = preferredSubmissionAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("No account is available for sending mail."), 7000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::NewMessage,
            .referenceEmailId = std::nullopt,
            .draftEmailId = std::nullopt,
            .initialTo = parsed->to,
            .initialCc = parsed->cc,
            .initialBcc = parsed->bcc,
            .initialSubject = parsed->subject,
            .initialBody = parsed->body,
            .useExistingWorkingCopy = false,
        });
    }

    void MainWindow::composeReply()
    {
        const auto accountId = activeAccountId();
        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a message to reply to."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::Reply,
            .referenceEmailId = *emailId,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::composeReplyAll()
    {
        const auto accountId = activeAccountId();
        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a message to reply to."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::ReplyAll,
            .referenceEmailId = *emailId,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::composeForward()
    {
        const auto accountId = activeAccountId();
        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a message to forward."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::Forward,
            .referenceEmailId = *emailId,
            .draftEmailId = std::nullopt,
            .initialTo = {},
        });
    }

    void MainWindow::editSelectedDraft()
    {
        const auto accountId = activeAccountId();
        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a draft to edit."), 5000);
            return;
        }

        const auto draftsMailbox = findMailboxByRole(m_mailboxReader, *accountId, "drafts");
        if (!activeMailboxId().has_value() || !draftsMailbox.has_value() ||
            draftsMailbox->id != *activeMailboxId())
        {
            m_statusBar->showMessage(i18n("Open a message from Drafts to edit it."), 5000);
            return;
        }

        openComposeForRequest({
            .accountId = *accountId,
            .mode = javelin::jmap::submission::ComposeMode::EditDraft,
            .referenceEmailId = std::nullopt,
            .draftEmailId = *emailId,
            .initialTo = {},
        });
    }

    const TabState* MainWindow::activeTab() const
    {
        return m_mailWorkspaceController->activeTab();
    }

    TabState* MainWindow::activeTab()
    {
        return m_mailWorkspaceController->activeTab();
    }

    bool MainWindow::activeTabIsMailbox() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && tabKind(*tab) == TabKind::Mailbox;
    }

    bool MainWindow::activeTabIsSearch() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && tabKind(*tab) == TabKind::Search;
    }

    bool MainWindow::activeTabIsCompose() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && tabKind(*tab) == TabKind::Compose;
    }

    bool MainWindow::activeTabIsContacts() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && tabKind(*tab) == TabKind::Contacts;
    }

    std::optional<std::string> MainWindow::activeAccountId() const
    {
        const auto* tab = activeTab();
        return tab == nullptr ? std::optional<std::string>{std::nullopt} : tabAccountId(*tab);
    }

    std::optional<std::string> MainWindow::preferredMailAccountId() const
    {
        if (auto accountId = activeAccountId(); accountId.has_value())
            return accountId;
        return currentAccountId(*m_mailboxView);
    }

    std::optional<std::string> MainWindow::preferredSubmissionAccountId() const
    {
        const auto preferredAccountId = preferredMailAccountId();
        const auto accountsResult = m_accountReader.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
            return std::nullopt;

        const auto usable = [](const auto& account)
        {
            return account.hasMailCapability && account.hasSubmissionCapability &&
                   !account.isReadOnly;
        };
        if (preferredAccountId.has_value())
        {
            const auto preferred = std::ranges::find(
                *accounts, *preferredAccountId, &javelin::jmap::cache::CachedAccount::accountId);
            if (preferred != accounts->end() && usable(*preferred))
                return preferred->accountId;
        }
        const auto primary = std::ranges::find_if(*accounts, [&usable](const auto& account)
                                                  { return account.isPrimary && usable(account); });
        if (primary != accounts->end())
            return primary->accountId;
        const auto fallback = std::ranges::find_if(*accounts, usable);
        return fallback == accounts->end() ? std::nullopt
                                           : std::optional<std::string>{fallback->accountId};
    }

    std::optional<std::string> MainWindow::activeMailboxId() const
    {
        const auto* tab = activeTab();
        return tab == nullptr ? std::optional<std::string>{std::nullopt} : tabMailboxId(*tab);
    }

    void MainWindow::updateTabBar()
    {
        m_tabBarPresenter->refresh(m_tabs, m_activeTabIndex);
    }

    void MainWindow::activateRelativeTab(const int offset)
    {
        if (m_tabs.size() < 2 || offset == 0)
            return;

        const int count = static_cast<int>(m_tabs.size());
        const int current = m_activeTabIndex.value_or(0);
        const int target = ((current + offset) % count + count) % count;
        activateTab(target, false);
    }

    void MainWindow::moveMessageSelection(const int direction, const bool unreadOnly)
    {
        if (direction == 0 || toolbarContextForActiveTab() != ToolbarContext::Mail ||
            m_messageModel->rowCount() <= 0)
        {
            return;
        }

        const auto current = m_messageView->currentIndex();
        int row =
            current.isValid() ? current.row() : (direction > 0 ? -1 : m_messageModel->rowCount());
        for (row += direction; row >= 0 && row < m_messageModel->rowCount(); row += direction)
        {
            const auto index = m_messageModel->index(row, 0);
            if (!index.isValid() ||
                (unreadOnly &&
                 !index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool()))
            {
                continue;
            }

            m_messageView->selectionModel()->setCurrentIndex(
                index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            m_messageView->scrollTo(index);
            return;
        }
    }

    void MainWindow::reopenLastClosedTab()
    {
        if (m_lastClosedTab == nullptr)
            return;

        auto persisted = m_lastClosedTab->tab;
        const auto previousTabCount = m_tabs.size();
        std::visit(
            [this](auto& tab)
            {
                using Persisted = std::decay_t<decltype(tab)>;
                if constexpr (std::is_same_v<Persisted, PersistedMailboxTab>)
                    restoreMailboxTab(tab);
                else if constexpr (std::is_same_v<Persisted, PersistedSearchTab>)
                    restoreSearchTab(std::move(tab));
                else if constexpr (std::is_same_v<Persisted, PersistedContactsTab>)
                    restoreContactsTab(tab);
                else if constexpr (std::is_same_v<Persisted, PersistedCalendarTab>)
                    m_calendarTabController->open(tab.displayedMonth);
            },
            persisted);

        if (m_tabs.size() <= previousTabCount)
            return;

        m_lastClosedTab.reset();
        m_reopenClosedTabAction->setEnabled(false);
        const int index = static_cast<int>(m_tabs.size() - 1);
        m_mailWorkspaceController->setActiveIndex(index);
        updateTabBar();
        activateTab(index, false);
    }

    MainWindow::ToolbarContext MainWindow::toolbarContextForActiveTab() const
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
        {
            return ToolbarContext::Mail;
        }

        switch (tabKind(*tab))
        {
        case TabKind::Mailbox:
        case TabKind::Search:
            return ToolbarContext::Mail;
        case TabKind::Compose:
            return ToolbarContext::Compose;
        case TabKind::Contacts:
            return ToolbarContext::Contacts;
        case TabKind::Calendar:
            return ToolbarContext::Calendar;
        }
        return ToolbarContext::Mail;
    }

    void MainWindow::updateActiveContextUi()
    {
        const auto context = toolbarContextForActiveTab();
        const bool mailContext = context == ToolbarContext::Mail;
        const bool contactsAvailable = m_contactsTabController->available();
        const bool calendarAvailable = m_calendarTabController->available();
        const auto selectedAccount = activeAccountId();
        const auto preferredMailAccount = preferredMailAccountId();
        const auto preferredSubmissionAccount = preferredSubmissionAccountId();
        m_closeTabAction->setEnabled(
            m_activeTabIndex.has_value() &&
            tabCanClose(m_tabs[static_cast<std::size_t>(*m_activeTabIndex)],
                        static_cast<std::size_t>(*m_activeTabIndex)));
        m_previousTabAction->setEnabled(m_tabs.size() > 1);
        m_nextTabAction->setEnabled(m_tabs.size() > 1);
        const auto canMoveMessageSelection =
            [this, mailContext](const int direction, const bool unreadOnly)
        {
            if (!mailContext || direction == 0 || m_messageModel->rowCount() <= 0)
                return false;
            const auto current = m_messageView->currentIndex();
            int row = current.isValid() ? current.row()
                                        : (direction > 0 ? -1 : m_messageModel->rowCount());
            for (row += direction; row >= 0 && row < m_messageModel->rowCount(); row += direction)
            {
                const auto index = m_messageModel->index(row, 0);
                if (!index.isValid())
                    continue;
                if (!unreadOnly ||
                    index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool())
                    return true;
            }
            return false;
        };
        m_previousMessageAction->setEnabled(canMoveMessageSelection(-1, false));
        m_nextMessageAction->setEnabled(canMoveMessageSelection(1, false));
        m_previousUnreadMessageAction->setEnabled(canMoveMessageSelection(-1, true));
        m_nextUnreadMessageAction->setEnabled(canMoveMessageSelection(1, true));
        m_focusMailboxTreeAction->setEnabled(mailContext);
        m_focusMessageListAction->setEnabled(mailContext);
        m_focusMessageReaderAction->setEnabled(mailContext);
        m_focusSearchAction->setEnabled(mailContext);
        m_contactsAction->setEnabled(contactsAvailable);
        m_calendarAction->setEnabled(calendarAvailable);
        m_newMessageAction->setEnabled(preferredSubmissionAccount.has_value());
        m_sendingIdentitiesAction->setEnabled(preferredSubmissionAccount.has_value());
        m_advancedSearchAction->setEnabled(preferredMailAccount.has_value());
        m_sieveAction->setEnabled(preferredMailAccount.has_value());
        m_refreshAction->setEnabled(context == ToolbarContext::Contacts ||
                                    context == ToolbarContext::Calendar ||
                                    preferredMailAccount.has_value());
        m_exportMailboxAction->setEnabled(mailContext && activeTabIsMailbox() &&
                                          activeMailboxId().has_value());
        m_exportAccountAction->setEnabled(preferredMailAccount.has_value());

        m_composeSendAction->setEnabled(false);
        m_composeScheduleSendAction->setEnabled(false);
        m_composeSaveDraftAction->setEnabled(false);
        m_composeAttachFilesAction->setEnabled(false);
        m_composeSignatureAction->setEnabled(false);
        m_composeRichTextAction->setEnabled(false);

        m_contactNewAction->setEnabled(contactsAvailable);
        m_contactEditAction->setEnabled(false);
        m_contactDeleteAction->setEnabled(false);
        m_contactCopyAction->setEnabled(false);
        m_contactImportAction->setEnabled(contactsAvailable);
        m_contactExportAction->setEnabled(false);
        m_contactDuplicatesAction->setEnabled(contactsAvailable);
        m_contactAddToGroupAction->setEnabled(false);
        m_contactRemoveFromGroupAction->setEnabled(false);
        m_contactManageAddressBooksAction->setEnabled(contactsAvailable);
        m_contactManageAddressBooksAction->setMenu(
            context == ToolbarContext::Contacts ? m_contactAddressBooksMenu : nullptr);
        m_contactRefreshAction->setEnabled(contactsAvailable);

        m_calendarNewEventAction->setEnabled(calendarAvailable);
        m_calendarPreviousMonthAction->setEnabled(calendarAvailable);
        m_calendarTodayAction->setEnabled(calendarAvailable);
        m_calendarNextMonthAction->setEnabled(calendarAvailable);
        m_calendarListAction->setEnabled(false);
        if (context != ToolbarContext::Calendar)
            m_calendarListAction->setMenu(nullptr);
        m_calendarRefreshAction->setEnabled(calendarAvailable);

        setToolBarVisible(QStringLiteral("mainToolBar"), context == ToolbarContext::Mail);
        setToolBarVisible(QStringLiteral("composeToolBar"), context == ToolbarContext::Compose);
        setToolBarVisible(QStringLiteral("contactsToolBar"), context == ToolbarContext::Contacts);
        setToolBarVisible(QStringLiteral("calendarToolBar"), context == ToolbarContext::Calendar);
        if (context == ToolbarContext::Mail)
        {
            m_mailActionController->update();
            m_newMessageAction->setEnabled(preferredSubmissionAccount.has_value());
            const bool activeAccountCanSubmit =
                selectedAccount.has_value() && preferredSubmissionAccount == selectedAccount;
            m_replyAction->setEnabled(m_replyAction->isEnabled() && activeAccountCanSubmit);
            m_replyAllAction->setEnabled(m_replyAllAction->isEnabled() && activeAccountCanSubmit);
            m_forwardAction->setEnabled(m_forwardAction->isEnabled() && activeAccountCanSubmit);
        }
        if (context == ToolbarContext::Compose)
        {
            m_saveCurrentAction->setText(i18nc("@action", "Save Draft"));
            m_saveCurrentAction->setEnabled(true);
            const auto state = m_composeTabController->toolbarState(activeTab());
            const QSignalBlocker blocker{m_composeRichTextAction};
            m_composeSendAction->setEnabled(state.canSend);
            m_composeScheduleSendAction->setVisible(state.canScheduleSend);
            m_composeScheduleSendAction->setEnabled(state.canScheduleSend);
            m_composeSaveDraftAction->setEnabled(state.canToggleRichText);
            m_composeAttachFilesAction->setEnabled(state.canToggleRichText);
            auto* signatureMenu = m_composeTabController->signatureMenuForTab(activeTab());
            m_composeSignatureAction->setMenu(signatureMenu);
            m_composeSignatureAction->setEnabled(state.canUseSignature && signatureMenu != nullptr);
            m_composeRichTextAction->setChecked(state.richText);
            m_composeRichTextAction->setEnabled(state.canToggleRichText);
        }
        if (context == ToolbarContext::Contacts)
        {
            const auto state = m_contactsTabController->toolbarState(activeTab());
            m_saveCurrentAction->setText(i18nc("@action", "Save Contact As…"));
            m_saveCurrentAction->setEnabled(state.canExportContact);
            m_contactNewAction->setEnabled(state.canCreateContact);
            m_contactEditAction->setEnabled(state.canEditContact);
            m_contactDeleteAction->setEnabled(state.canDeleteContact);
            m_contactCopyAction->setEnabled(state.canCopyContact);
            m_contactImportAction->setEnabled(state.canCreateContact);
            m_contactExportAction->setEnabled(state.canExportContact);
            m_contactDuplicatesAction->setEnabled(state.canFindDuplicates);
            m_contactAddToGroupAction->setEnabled(state.canAddToGroup);
            m_contactRemoveFromGroupAction->setEnabled(state.canRemoveFromGroup);
            m_contactManageAddressBooksAction->setEnabled(state.canManageAddressBooks);
            m_contactRefreshAction->setEnabled(state.canRefresh);
            m_refreshAction->setEnabled(state.canRefresh);
            if (state.canAddToGroup)
                m_contactsTabController->populateAddToGroupMenu(activeTab(),
                                                                *m_contactAddToGroupMenu);
            if (state.canRemoveFromGroup)
                m_contactsTabController->populateRemoveFromGroupMenu(activeTab(),
                                                                     *m_contactRemoveFromGroupMenu);
            if (state.canManageAddressBooks)
                m_contactsTabController->populateAddressBookMenu(activeTab(),
                                                                 *m_contactAddressBooksMenu);
        }
        if (context == ToolbarContext::Calendar)
        {
            m_saveCurrentAction->setText(i18nc("@action", "Save"));
            m_saveCurrentAction->setEnabled(false);
            auto* menu = m_calendarTabController->calendarMenuForTab(activeTab());
            m_calendarListAction->setMenu(menu);
            m_calendarListAction->setEnabled(menu != nullptr);
        }
    }

    void MainWindow::saveNewToolbarConfig()
    {
        KXmlGuiWindow::saveNewToolbarConfig();
        m_emailContextMenu = qobject_cast<QMenu*>(
            guiFactory()->container(QStringLiteral("email_context_menu"), this));
        if (m_emailContextMenu != nullptr)
        {
            m_mailActionController->configureContextMenu(
                *m_emailContextMenu,
                [this] { return m_settings.workspaceSettings().emailContextMenuLayout; },
                [this](const QList<QAction*>& actions)
                {
                    unplugActionList(QStringLiteral("email_context_menu_layout"));
                    if (!actions.empty())
                        plugActionList(QStringLiteral("email_context_menu_layout"), actions);
                });
        }
        updateActiveContextUi();
    }

    void MainWindow::openOrActivateMailboxTab(std::string accountId, std::string mailboxId,
                                              const QString title, std::optional<std::string> role,
                                              const bool refreshRemote)
    {
        const auto index = m_mailWorkspaceController->openMailbox(
            std::move(accountId), std::move(mailboxId), title, std::move(role));
        updateTabBar();
        activateTab(index, refreshRemote);
    }

    void MainWindow::activateMailboxInHomeTab(std::string accountId, std::string mailboxId,
                                              QString title, std::optional<std::string> role,
                                              const bool refreshRemote)
    {
        const auto index = m_mailWorkspaceController->activateHomeMailbox(
            std::move(accountId), std::move(mailboxId), std::move(title), std::move(role));
        updateTabBar();
        activateTab(index, refreshRemote);
    }

    void MainWindow::openOrActivateSearchTab(std::string accountId, QString query,
                                             const bool refreshRemote)
    {
        openOrActivateSearchTab(
            std::move(accountId),
            javelin::jmap::search::EmailSearchCriteria{.text = query.toStdString()}, refreshRemote);
    }

    void MainWindow::openOrActivateSearchTab(std::string accountId,
                                             javelin::jmap::search::EmailSearchCriteria criteria,
                                             const bool refreshRemote)
    {
        const auto index =
            m_mailWorkspaceController->openSearch(std::move(accountId), std::move(criteria));
        updateTabBar();
        activateTab(index, refreshRemote);
    }

    void MainWindow::activateTab(const int index, const bool refreshRemote)
    {
        javelin::app::PerformanceSpan metrics{
            QStringLiteral("gui"), QStringLiteral("tab_activation"),
            QStringLiteral("index=%1 refresh_remote=%2").arg(index).arg(refreshRemote)};
        QElapsedTimer timer;
        timer.start();
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            const auto plan = planTabActivation({});
            m_mailWorkspaceController->setActiveIndex(std::nullopt);
            m_messageModel->clear();
            setMessageViewSelection(std::nullopt, std::nullopt, std::nullopt);
            if (m_contentStack != nullptr)
                m_contentStack->setCurrentIndex(0);
            if (m_mailboxPane != nullptr)
                m_mailboxPane->setVisible(plan.showMailboxPane);
            updateTabBar();
            updateEmptyStates();
            updateMessageListHeader();
            m_quickFilterController->activate(nullptr);
            m_mailActionController->activate(nullptr);
            updateActiveContextUi();
            metrics.finish(QStringLiteral("empty"));
            return;
        }

        m_mailWorkspaceController->setActiveIndex(index);
        auto& tab = m_tabs[static_cast<std::size_t>(index)];
        m_quickFilterController->activate(&tab);
        m_mailActionController->activate(&tab);
        const auto initialPlan = planTabActivation({
            .kind = tabKind(tab),
            .homeTab = index == 0,
            .messageListStale = m_mailWorkspaceController->stateStale(tab),
            .remoteRefreshRequested = refreshRemote,
        });

        updateActiveContextUi();
        if (m_tabBar->currentIndex() != index)
        {
            QSignalBlocker blocker{m_tabBar};
            m_tabBar->setCurrentIndex(index);
        }
        if (auto* composeWidget = m_composeTabController->contentWidgetForTab(&tab);
            composeWidget != nullptr)
        {
            m_contentStack->setCurrentWidget(composeWidget);
        }
        else if (auto* contactsWidget = m_contactsTabController->contentWidgetForTab(&tab);
                 contactsWidget != nullptr)
        {
            m_contentStack->setCurrentWidget(contactsWidget);
        }
        else if (auto* calendarWidget = m_calendarTabController->contentWidgetForTab(&tab);
                 calendarWidget != nullptr)
        {
            m_contentStack->setCurrentWidget(calendarWidget);
        }
        else if (m_contentStack != nullptr)
        {
            m_contentStack->setCurrentIndex(0);
        }

        m_messageListTabBindingPresenter->syncNavigation(&tab, initialPlan.showMailboxPane);
        loadActiveTabFromCache(false, false);
        const auto loadedPlan = planTabActivation({
            .kind = tabKind(tab),
            .homeTab = index == 0,
            .messageListStale = m_mailWorkspaceController->stateStale(tab),
            .remoteRefreshRequested = refreshRemote,
        });
        if (loadedPlan.refreshRemote)
        {
            QTimer::singleShot(
                100, this,
                [this, index, refreshRemote]
                {
                    if (m_activeTabIndex != std::optional<int>{index} || index < 0 ||
                        static_cast<std::size_t>(index) >= m_tabs.size())
                        return;
                    const auto& currentTab = m_tabs[static_cast<std::size_t>(index)];
                    const auto plan = planTabActivation({
                        .kind = tabKind(currentTab),
                        .homeTab = index == 0,
                        .messageListStale = m_mailWorkspaceController->stateStale(currentTab),
                        .remoteRefreshRequested = refreshRemote,
                    });
                    if (plan.refreshRemote)
                        refreshTabFromServer(static_cast<std::size_t>(index));
                });
        }

        qCDebug(logGuiMailbox).noquote() << "activate tab" << index << "refreshRemote"
                                         << refreshRemote << "ms" << timer.elapsed();
        metrics.finish(QStringLiteral("completed"));
    }

    void MainWindow::closeTab(const int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            return;
        }
        auto& tab = m_tabs[static_cast<std::size_t>(index)];
        if (!tabCanClose(tab, static_cast<std::size_t>(index)))
        {
            return;
        }

        const auto kind = tabKind(tab);
        std::unique_ptr<ClosedTabState> closedTab;
        if (kind != TabKind::Compose)
            closedTab = std::make_unique<ClosedTabState>(ClosedTabState{persistTab(tab)});

        if (kind == TabKind::Compose && !closeComposeTab(index))
        {
            return;
        }

        if (kind == TabKind::Contacts && !m_contactsTabController->close(tab))
        {
            return;
        }

        if (kind == TabKind::Calendar)
            static_cast<void>(m_calendarTabController->close(tab));

        if (!m_mailWorkspaceController->eraseTab(index))
            return;

        m_lastClosedTab = std::move(closedTab);
        m_reopenClosedTabAction->setEnabled(m_lastClosedTab != nullptr);
        if (!m_activeTabIndex.has_value())
        {
            activateTab(-1, false);
            return;
        }

        updateTabBar();
        activateTab(*m_activeTabIndex, false);
    }

    void
    MainWindow::applyActiveTabItemsPreservingSelection(const std::optional<int> previousMessageRow)
    {
        bool restoredSelectionChanged = false;
        const bool wasUpdatingModel = m_modelUpdateInProgress;
        m_modelUpdateInProgress = true;
        {
            QSignalBlocker messageSelectionBlocker{m_messageView->selectionModel()};
            m_messageListTabBindingPresenter->applyItems(activeTab());
            restoredSelectionChanged =
                m_messageSelectionController->restoreTabSelection(activeTab(), previousMessageRow);
        }
        m_modelUpdateInProgress = wasUpdatingModel;
        const auto* tab = activeTab();
        const auto* mailbox =
            tab == nullptr ? nullptr : std::get_if<MailboxTabState>(&tab->content);
        const bool quickFilterActive = mailbox != nullptr && mailbox->session != nullptr &&
                                       mailbox->session->quickFilterActive();
        if (javelin::gui::messages::shouldActivateRestoredSelection(restoredSelectionChanged,
                                                                    quickFilterActive))
            handleCurrentMessageChanged(m_messageView->currentIndex());
        else
            refreshSelectionFromModels();
        updateActiveContextUi();
    }

    void MainWindow::loadActiveTabFromCache(const bool forceReload, const bool refreshRemote)
    {
        javelin::app::PerformanceSpan metrics{QStringLiteral("gui"),
                                              QStringLiteral("cached_view_activation"),
                                              QStringLiteral("force_reload=%1 refresh_remote=%2")
                                                  .arg(forceReload)
                                                  .arg(refreshRemote)};
        QElapsedTimer timer;
        timer.start();
        auto* tab = activeTab();
        if (tab == nullptr)
        {
            m_messageListTabBindingPresenter->applyItems(nullptr);
            refreshSelectionFromModels();
            metrics.finish(QStringLiteral("empty"));
            return;
        }

        if (!m_mailWorkspaceController->loadCachedState(*tab, forceReload))
        {
            const auto plan = planTabActivation({
                .kind = tabKind(*tab),
                .homeTab = m_activeTabIndex == std::optional<int>{0},
            });
            if (plan.clearMessagePresentation)
            {
                m_messageModel->clear();
                setMessageViewSelection(std::nullopt, std::nullopt, std::nullopt);
            }
            m_mailActionController->update();
            metrics.finish(QStringLiteral("cache_miss"));
            return;
        }

        const auto cacheMilliseconds = timer.restart();
        applyActiveTabItemsPreservingSelection(m_messageSelectionController->currentRow());
        const auto applyMilliseconds = timer.elapsed();
        if (cacheMilliseconds + applyMilliseconds >= 50)
        {
            qCWarning(logGuiMailbox).noquote()
                << "slow cached tab activation cacheMs" << cacheMilliseconds << "applyMs"
                << applyMilliseconds;
        }
        metrics.finish(QStringLiteral("loaded"), QStringLiteral("cache_ms=%1 apply_ms=%2")
                                                     .arg(cacheMilliseconds)
                                                     .arg(applyMilliseconds));
        if (m_mailWorkspaceController->stateStale(*tab) ||
            (refreshRemote && tabKind(*tab) == TabKind::Mailbox))
        {
            const auto mode = refreshRemote && tabKind(*tab) == TabKind::Mailbox
                                  ? javelin::app::MessageListRefreshMode::RefreshFromServer
                                  : javelin::app::MessageListRefreshMode::Materialize;
            static_cast<void>(m_mailWorkspaceController->refresh(*tab, mode));
        }
    }

    void MainWindow::refreshActiveTabFromServer()
    {
        if (!m_activeTabIndex.has_value() || *m_activeTabIndex < 0 ||
            static_cast<std::size_t>(*m_activeTabIndex) >= m_tabs.size())
        {
            return;
        }

        refreshTabFromServer(static_cast<std::size_t>(*m_activeTabIndex));
    }

    void MainWindow::refreshTabFromServer(const std::size_t tabIndex)
    {
        if (tabIndex >= m_tabs.size())
        {
            return;
        }

        auto& tab = m_tabs[tabIndex];
        if (m_mailWorkspaceController->refresh(
                tab, javelin::app::MessageListRefreshMode::RefreshFromServer))
            return;

        if (m_contactsTabController->refresh(&tab))
            return;
        static_cast<void>(m_calendarTabController->refresh(&tab));
    }

    void MainWindow::activateMailboxSelection(const bool refreshRemote)
    {
        javelin::app::PerformanceSpan metrics{
            QStringLiteral("gui"), QStringLiteral("mailbox_navigation"),
            QStringLiteral("refresh_remote=%1").arg(refreshRemote)};
        QElapsedTimer timer;
        timer.start();
        const auto accountId = currentAccountId(*m_mailboxView);
        const auto mailboxId = currentMailboxId(*m_mailboxView);
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            metrics.finish(QStringLiteral("no_selection"));
            return;
        }

        m_messageNavigationPort.cancel();

        const auto currentIndex = m_mailboxView->currentIndex();
        activateMailboxInHomeTab(
            *accountId, *mailboxId,
            currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxNameRole)
                .toString(),
            currentMailboxRole(*m_mailboxView), refreshRemote);
        metrics.finish(QStringLiteral("completed"),
                       QStringLiteral("elapsed_ms=%1").arg(timer.elapsed()));
    }

    void
    MainWindow::markTabsStaleForAccount(const std::string_view accountId,
                                        const std::optional<std::string_view> refreshedMailboxId)
    {
        m_mailWorkspaceController->markTabsStaleForAccount(accountId, refreshedMailboxId);
    }

    void MainWindow::markSearchTabsStaleForAccount(const std::string_view accountId)
    {
        m_mailWorkspaceController->markSearchTabsStaleForAccount(accountId);
    }

    void MainWindow::openMailboxSelectionInTab(const bool refreshRemote)
    {
        const auto accountId = currentAccountId(*m_mailboxView);
        const auto mailboxId = currentMailboxId(*m_mailboxView);
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            return;
        }

        const auto currentIndex = m_mailboxView->currentIndex();
        const auto title =
            currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxNameRole)
                .toString();
        const auto role = currentMailboxRole(*m_mailboxView);
        const auto index =
            m_mailWorkspaceController->openMailbox(*accountId, *mailboxId, title, role, 1);
        updateTabBar();
        activateTab(index, refreshRemote);
    }

    void MainWindow::openComposeForRequest(javelin::jmap::submission::OpenComposeRequest request)
    {
        m_composeTabController->open(std::move(request));
    }

    void MainWindow::restoreDraft(const QString& accountId, const QString& draftEmailId,
                                  const QString& composeSessionId)
    {
        openComposeForRequest({
            .accountId = accountId.toStdString(),
            .mode = javelin::jmap::submission::ComposeMode::EditDraft,
            .referenceEmailId = std::nullopt,
            .draftEmailId = draftEmailId.toStdString(),
            .initialTo = {},
            .useExistingWorkingCopy = true,
            .composeSessionId = composeSessionId.toStdString(),
        });
    }

    bool MainWindow::closeComposeTab(const int index)
    {
        const auto input = m_composeTabController->closeInput(index);
        if (!input.has_value())
            return false;

        switch (planComposeTabClose(*input))
        {
        case ComposeTabClosePlan::BlockWhileBusy:
            m_statusBar->showMessage(
                i18n("Wait for the current compose operation to finish first."), 5000);
            return false;
        case ComposeTabClosePlan::CloseImmediately:
            return m_composeTabController->closeImmediately(index);
        case ComposeTabClosePlan::DiscardWorkingCopyAndClose:
            return m_composeTabController->discardAndClose(index);
        case ComposeTabClosePlan::ConfirmKeepSavedDraft:
        {
            QMessageBox messageBox{this};
            messageBox.setWindowTitle(i18n("Close Draft"));
            messageBox.setText(
                i18n("This message is already saved in Drafts. Close the tab and keep it there?"));
            messageBox.setInformativeText(m_composeTabController->confirmationDetails(index));
            QAbstractButton* keepDraftButton =
                messageBox.addButton(i18n("Keep Draft"), QMessageBox::AcceptRole);
            messageBox.addButton(QMessageBox::Cancel);
            messageBox.exec();
            if (messageBox.clickedButton() != keepDraftButton)
                return false;
            return m_composeTabController->discardAndClose(index);
        }
        case ComposeTabClosePlan::ConfirmSaveOrDiscard:
        {
            QMessageBox messageBox{this};
            messageBox.setWindowTitle(i18n("Save Changes?"));
            messageBox.setText(i18n("Save changes to this draft before closing?"));
            messageBox.setInformativeText(m_composeTabController->confirmationDetails(index));
            QAbstractButton* saveButton =
                messageBox.addButton(i18nc("@action:button", "Save"), QMessageBox::AcceptRole);
            QAbstractButton* discardButton = messageBox.addButton(
                i18nc("@action:button", "Don't Save"), QMessageBox::DestructiveRole);
            messageBox.addButton(QMessageBox::Cancel);
            messageBox.exec();
            if (messageBox.clickedButton() == saveButton)
            {
                m_composeTabController->saveDraftAndClose(index);
                return false;
            }
            if (messageBox.clickedButton() != discardButton)
                return false;
            return m_composeTabController->discardAndClose(index);
        }
        }
        return false;
    }

    void MainWindow::executeSearch(const QString& text)
    {
        const QString trimmed = text.trimmed();
        if (trimmed.isEmpty())
        {
            clearSearch();
            return;
        }

        const auto accountId = preferredMailAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("Select an account before searching."), 5000);
            return;
        }

        openOrActivateSearchTab(*accountId, trimmed, true);
    }

    void MainWindow::showAdvancedSearch()
    {
        const auto accountId = preferredMailAccountId();
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("Select an account before searching."), 5000);
            return;
        }

        javelin::gui::search::AdvancedSearchDialog dialog{this};
        if (dialog.exec() != QDialog::Accepted)
        {
            return;
        }

        auto criteria = dialog.criteria();
        if (javelin::jmap::search::isEmpty(criteria))
        {
            m_statusBar->showMessage(i18n("Enter at least one search field."), 5000);
            return;
        }

        openOrActivateSearchTab(*accountId, std::move(criteria), true);
    }

    void MainWindow::clearSearch()
    {
        const auto* tab = activeTab();
        if (tab != nullptr && std::holds_alternative<SearchTabState>(tab->content))
        {
            closeTab(*m_activeTabIndex);
        }
        else
        {
            m_mailboxSearchEdit->clear();
        }
    }

    void MainWindow::showSortMenu()
    {
        QMenu menu{this};
        const auto addPropertyAction =
            [this, &menu](const QString& label,
                          const javelin::jmap::query::EmailListSortProperty property)
        {
            auto* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(m_emailListSort.property == property);
            connect(action, &QAction::triggered, this,
                    [this, property]
                    {
                        setEmailListSort(javelin::jmap::query::EmailListSort{
                            .property = property,
                            .direction = m_emailListSort.direction,
                        });
                    });
        };

        addPropertyAction(i18n("Date received"),
                          javelin::jmap::query::EmailListSortProperty::ReceivedAt);
        addPropertyAction(i18n("Date sent"), javelin::jmap::query::EmailListSortProperty::SentAt);
        addPropertyAction(i18nc("@item message sort property", "From"),
                          javelin::jmap::query::EmailListSortProperty::From);
        addPropertyAction(i18nc("@item message sort property", "To"),
                          javelin::jmap::query::EmailListSortProperty::To);
        addPropertyAction(i18nc("@item message sort property", "Subject"),
                          javelin::jmap::query::EmailListSortProperty::Subject);
        addPropertyAction(i18nc("@item message sort property", "Size"),
                          javelin::jmap::query::EmailListSortProperty::Size);
        menu.addSeparator();

        const auto addDirectionAction =
            [this, &menu](const QString& label,
                          const javelin::jmap::query::EmailListSortDirection direction)
        {
            auto* action = menu.addAction(label);
            action->setCheckable(true);
            action->setChecked(m_emailListSort.direction == direction);
            connect(action, &QAction::triggered, this,
                    [this, direction]
                    {
                        setEmailListSort(javelin::jmap::query::EmailListSort{
                            .property = m_emailListSort.property,
                            .direction = direction,
                        });
                    });
        };

        addDirectionAction(i18nc("@item message sort direction", "Descending"),
                           javelin::jmap::query::EmailListSortDirection::Descending);
        addDirectionAction(i18nc("@item message sort direction", "Ascending"),
                           javelin::jmap::query::EmailListSortDirection::Ascending);

        menu.exec(m_messageSortButton->mapToGlobal(QPoint{0, m_messageSortButton->height()}));
    }

    void MainWindow::setEmailListSort(javelin::jmap::query::EmailListSort sort)
    {
        if (m_emailListSort.property == sort.property &&
            m_emailListSort.direction == sort.direction)
        {
            return;
        }

        m_mailWorkspaceController->setSort(std::move(sort));
        auto workspace = m_settings.workspaceSettings();
        auto persisted = deserializeMainWindowState(workspace.mainWindowState, m_emailListSort);
        persisted.emailListSort = m_emailListSort;
        workspace.mainWindowState = serializeMainWindowState(persisted);
        if (const auto error = m_settings.updateWorkspace(std::move(workspace)))
        {
            qWarning().noquote() << QStringLiteral("Could not save message-list sort:")
                                 << error->detail;
            m_statusBar->showMessage(i18n("Could not save the message-list sort."), 5000);
        }

        updateSortButton();
        loadActiveTabFromCache(true);
        updateMessageListHeader();
        m_statusBar->showMessage(i18n("Sorting by %1, %2.",
                                      sortPropertyLabel(m_emailListSort.property),
                                      sortDirectionLabel(m_emailListSort.direction)),
                                 5000);
    }

    void MainWindow::maybeLoadMoreMessages()
    {
        auto* tab = activeTab();
        if (tab == nullptr || m_messageView == nullptr || m_messageModel == nullptr)
            return;
        const auto* session = messageListSession(*tab);
        if (session == nullptr || !session->state().loadMoreError.isEmpty() ||
            !m_mailWorkspaceController->canLoadMore(*tab))
        {
            return;
        }

        const auto* scrollBar = m_messageView->verticalScrollBar();
        if (scrollBar == nullptr || m_messageModel->rowCount() == 0)
            return;
        if (javelin::gui::messages::shouldLoadMoreMessages(scrollBar->value(), scrollBar->maximum(),
                                                           scrollBar->pageStep(),
                                                           m_messageModel->rowCount()))
        {
            static_cast<void>(m_mailWorkspaceController->loadMore(*tab));
        }
    }

    void MainWindow::loadMoreMessages()
    {
        auto* tab = activeTab();
        if (tab == nullptr)
            return;
        static_cast<void>(m_mailWorkspaceController->loadMore(*tab));
    }

    void MainWindow::reloadAccounts()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        if (m_contactsAction != nullptr)
            updateActiveContextUi();
    }

    void MainWindow::refreshViewsFromCache()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        loadActiveTabFromCache(true);
    }

    void MainWindow::setMessageViewSelection(std::optional<std::string> accountId,
                                             std::optional<std::string> mailboxId,
                                             std::optional<std::string> emailId)
    {
        std::optional<std::string> junkMailboxId;
        if (accountId.has_value() && m_mailboxModel != nullptr)
        {
            const auto junkIndex = javelin::gui::mailboxes::findMailboxIndexForRole(
                *m_mailboxModel, QString::fromStdString(*accountId), QStringLiteral("junk"));
            if (junkIndex.isValid())
            {
                const auto id =
                    junkIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole)
                        .toString();
                if (!id.isEmpty())
                {
                    junkMailboxId = id.toStdString();
                }
            }
        }
        m_messageViewContainer->setSelection(m_messageViewReader, std::move(accountId),
                                             std::move(mailboxId), std::move(emailId),
                                             std::move(junkMailboxId));
    }

    void MainWindow::refreshSelectionFromModels()
    {
        if (activeTabIsCompose())
        {
            m_mailActionController->update();
            return;
        }

        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        m_quickFilterController->syncContinuitySelection(
            m_messageSelectionController->currentEmailId(),
            m_messageSelectionController->currentThreadId());
        auto selectedSummaries = m_messageSelectionController->selectedMessageSummaries();
        if (selectedSummaries.size() > 1)
        {
            m_messageViewContainer->setMultipleSelection(accountId, mailboxId,
                                                         std::move(selectedSummaries));
            m_messageSelectionController->syncTabSelection(activeTab());
            updateEmptyStates();
            updateMessageListHeader();
            m_mailActionController->update();
            return;
        }

        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!emailId.has_value())
        {
            if (const auto* route = m_messageNavigationController->activeRoute(activeTab()))
            {
                setMessageViewSelection(route->accountId, route->mailboxId, route->emailId);
                updateEmptyStates();
                updateMessageListHeader();
                m_mailActionController->update();
                return;
            }
        }
        setMessageViewSelection(accountId, mailboxId, emailId);
        m_messageSelectionController->syncTabSelection(activeTab());
        updateEmptyStates();
        updateMessageListHeader();
        m_mailActionController->update();
    }

    void MainWindow::selectPendingInitialMailbox()
    {
        if (!m_pendingInitialMailboxAccountId.has_value())
            return;

        if (!m_tabs.empty() || currentMailboxId(*m_mailboxView).has_value())
        {
            m_pendingInitialMailboxAccountId.reset();
            return;
        }

        const auto index = javelin::gui::mailboxes::findMailboxIndexForRole(
            *m_mailboxModel, QString::fromStdString(*m_pendingInitialMailboxAccountId),
            QStringLiteral("inbox"));
        if (!index.isValid())
            return;

        m_pendingInitialMailboxAccountId.reset();
        m_mailboxView->setCurrentIndex(index);
        m_mailboxView->scrollTo(index);
    }

    void MainWindow::updateEmptyStates()
    {
        std::optional<javelin::app::MailAccountStatus> accountStatus;
        if (const auto accountId = activeAccountId(); accountId.has_value())
        {
            if (const auto found = m_accountStatuses.find(*accountId);
                found != m_accountStatuses.end())
            {
                accountStatus = found->second;
            }
        }
        m_messageEmptyStateAction = m_messageListTabPresenter->showEmptyState(
            activeTab(), static_cast<std::size_t>(m_messageModel->rowCount()), accountStatus);
    }

    void MainWindow::activateMessageListEmptyAction()
    {
        using javelin::gui::messages::MessageListEmptyAction;
        switch (m_messageEmptyStateAction)
        {
        case MessageListEmptyAction::None:
            return;
        case MessageListEmptyAction::ClearFilters:
            m_quickFilterController->clear();
            return;
        case MessageListEmptyAction::SearchServer:
            if (auto* tab = activeTab(); tab != nullptr)
                static_cast<void>(m_mailWorkspaceController->promoteSearch(*tab));
            return;
        case MessageListEmptyAction::EditSearch:
            editActiveSearch();
            return;
        case MessageListEmptyAction::Retry:
            refreshActiveTabFromServer();
            return;
        case MessageListEmptyAction::SignInAgain:
            if (const auto accountId = activeAccountId(); accountId.has_value())
            {
                m_authenticationPromptCoordinator->signInAgainForAccount(
                    QString::fromStdString(*accountId));
            }
            return;
        }
    }

    void MainWindow::editActiveSearch()
    {
        auto* tab = activeTab();
        auto* search = tab == nullptr ? nullptr : std::get_if<SearchTabState>(&tab->content);
        if (search == nullptr || search->session == nullptr || !m_activeTabIndex.has_value())
            return;

        javelin::gui::search::AdvancedSearchDialog dialog{search->session->criteria(), this};
        if (dialog.exec() != QDialog::Accepted)
            return;

        auto criteria = dialog.criteria();
        if (javelin::jmap::search::isEmpty(criteria))
        {
            m_statusBar->showMessage(i18n("Enter at least one search field."), 5000);
            return;
        }

        const int oldIndex = *m_activeTabIndex;
        const int newIndex = m_mailWorkspaceController->openSearch(search->session->accountId(),
                                                                   std::move(criteria));
        if (newIndex != oldIndex)
        {
            closeTab(oldIndex);
            return;
        }

        updateTabBar();
        activateTab(newIndex, true);
    }

    void MainWindow::updateMessageListHeader()
    {
        m_messageListTabPresenter->showHeader(activeTab());
    }

    void MainWindow::updateSortButton()
    {
        if (m_messageSortButton == nullptr)
        {
            return;
        }

        const auto description =
            i18n("Sort messages: %1, %2", sortPropertyLabel(m_emailListSort.property),
                 sortDirectionLabel(m_emailListSort.direction));
        m_messageSortButton->setAccessibleName(description);
        m_messageSortButton->setToolTip(description);
    }

    bool MainWindow::eventFilter(QObject* watched, QEvent* event)
    {
        if (event->type() == QEvent::KeyRelease || event->type() == QEvent::InputMethod ||
            event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
            event->type() == QEvent::MouseButtonRelease)
        {
            QTimer::singleShot(0, this, &MainWindow::updateUndoRedoActions);
        }

        if (watched == m_tabBar && event->type() == QEvent::MouseButtonRelease)
        {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::MiddleButton)
            {
                const int index = m_tabBar->tabAt(mouseEvent->position().toPoint());
                if (index >= 0)
                {
                    closeTab(index);
                    return true;
                }
            }
        }

        if (m_messageView != nullptr && watched == m_messageView->viewport() &&
            event->type() == QEvent::MouseButtonPress)
        {
            const auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::RightButton)
            {
                const QModelIndex index = m_messageView->indexAt(mouseEvent->position().toPoint());
                if (index.isValid() && m_messageView->selectionModel()->isSelected(index))
                {
                    return true;
                }
            }
        }

        if (watched == m_messageView && event->type() == QEvent::KeyPress)
        {
            const auto* keyEvent = static_cast<QKeyEvent*>(event);
            const QModelIndex currentIndex = m_messageView->currentIndex();
            if (!currentIndex.isValid())
            {
                return KXmlGuiWindow::eventFilter(watched, event);
            }

            const bool controlOnly = keyEvent->modifiers() == Qt::ControlModifier;
            if (controlOnly && (keyEvent->key() == Qt::Key_Up || keyEvent->key() == Qt::Key_Down))
            {
                const int direction = keyEvent->key() == Qt::Key_Up ? -1 : 1;
                const int nextRow = currentIndex.row() + direction;
                if (nextRow < 0 || nextRow >= m_messageModel->rowCount())
                {
                    return true;
                }

                const QModelIndex nextIndex = m_messageModel->index(nextRow, 0);
                if (!nextIndex.isValid())
                {
                    return true;
                }

                m_messageView->selectionModel()->setCurrentIndex(
                    nextIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_messageView->scrollTo(nextIndex);
                return true;
            }

            const auto threadId =
                currentIndex.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                    .toString();
            if (threadId.isEmpty())
            {
                return KXmlGuiWindow::eventFilter(watched, event);
            }

            if (keyEvent->key() == Qt::Key_Right)
            {
                if (m_messageModel->setThreadExpanded(threadId.toStdString(), true))
                {
                    return true;
                }
            }

            if (keyEvent->key() == Qt::Key_Left)
            {
                if (m_messageModel->setThreadExpanded(threadId.toStdString(), false))
                {
                    const auto summaryEmailId =
                        m_messageModel->summaryEmailIdForThread(threadId.toStdString());
                    if (summaryEmailId.has_value())
                    {
                        m_messageSelectionController->restoreSelection(
                            activeAccountId(), activeMailboxId(),
                            std::optional<std::string>{threadId.toStdString()}, summaryEmailId);
                    }
                    return true;
                }
            }
        }

        return KXmlGuiWindow::eventFilter(watched, event);
    }

    void MainWindow::openDeveloperOptions()
    {
        if (m_developerOptionsDialog == nullptr)
        {
            m_developerOptionsDialog = new javelin::gui::developer::DeveloperOptionsDialog(
                m_developerDiagnosticsPort, m_developerMaintenancePort, this);
            m_developerOptionsDialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(m_developerOptionsDialog, &QObject::destroyed, this,
                    [this] { m_developerOptionsDialog = nullptr; });
        }
        m_developerOptionsDialog->show();
        m_developerOptionsDialog->raise();
        m_developerOptionsDialog->activateWindow();
    }

    void MainWindow::openPreferences()
    {
        openPreferencesForConnection({});
    }

    void MainWindow::configureEmailContextMenu()
    {
        EmailContextMenuEditorDialog dialog{m_settings, *actionCollection(), this};
        dialog.exec();
    }

    void MainWindow::configureCalendarEventContextMenu()
    {
        javelin::gui::calendar::CalendarEventContextMenuEditorDialog dialog{
            m_settings, *actionCollection(), this};
        dialog.exec();
    }

    void MainWindow::openPreferencesForConnection(const QString& connectionId)
    {
        javelin::gui::settings::PreferencesDialog dialog{m_settings,
                                                         m_accountCommandPort,
                                                         m_mailCommandPort,
                                                         m_onboardingPort,
                                                         m_translationService,
                                                         m_accountReader,
                                                         m_mailboxReader,
                                                         m_developerDiagnosticsPort,
                                                         m_developerMaintenancePort,
                                                         this};
        connect(&dialog, &javelin::gui::settings::PreferencesDialog::accountAdded, this,
                [this](const javelin::gui::settings::ConnectionSettings& settings)
                { m_accountRefreshController->refreshConnection(settings); });
        connect(&dialog, &javelin::gui::settings::PreferencesDialog::accountReauthenticated, this,
                [this](const javelin::gui::settings::ConnectionSettings& settings)
                { m_accountRefreshController->refreshConnection(settings); });
        if (!connectionId.isEmpty())
            dialog.selectConfiguredAccount(connectionId);
        if (dialog.exec() == QDialog::Accepted)
        {
            m_messageViewContainer->appearanceSettingsChanged();
            m_messageViewContainer->translationSettingsChanged();
            m_statusBar->showMessage(i18n("Saved preferences."), 3000);
            m_mailboxModel->refresh();
            const auto accountId = preferredMailAccountId();
            if (accountId.has_value())
            {
                refreshAccountFromServer(*accountId);
            }
        }
    }

    void MainWindow::refreshFromServer()
    {
        if (toolbarContextForActiveTab() == ToolbarContext::Contacts)
        {
            m_contactsTabController->invoke(activeTab(), ContactsTabCommand::Refresh);
            return;
        }
        if (toolbarContextForActiveTab() == ToolbarContext::Calendar)
        {
            m_calendarTabController->invoke(activeTab(), CalendarTabCommand::Refresh);
            return;
        }
        if (activeTabIsSearch())
        {
            refreshActiveTabFromServer();
            return;
        }

        const auto accountId = preferredMailAccountId();
        if (!accountId.has_value())
        {
            presentUserInterventionError(i18n("Select an account to refresh."));
            return;
        }
        refreshAccountFromServer(*accountId);
    }

    void MainWindow::refreshAccountFromServer(std::string accountId)
    {
        m_accountRefreshController->refreshAccount(std::move(accountId));
    }

    void MainWindow::findConversationsWithSender(const QModelIndex& index)
    {
        const auto accountId = activeAccountId();
        const auto senderEmail =
            index.data(javelin::gui::messages::MessageListModel::SenderEmailRole)
                .toString()
                .trimmed();
        if (!accountId.has_value() || senderEmail.isEmpty())
        {
            m_statusBar->showMessage(i18n("No sender address is available."), 5000);
            return;
        }

        openOrActivateSearchTab(
            *accountId,
            javelin::jmap::search::EmailSearchCriteria{.with = senderEmail.toStdString()}, true);
    }

    void MainWindow::showMailboxContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_mailboxView->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        const auto accountId =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::AccountIdRole).toString();
        const auto mailboxId =
            index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxIdRole).toString();
        if (accountId.isEmpty())
        {
            return;
        }
        if (index.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxPendingCreateRole)
                .toBool())
            return;

        m_mailboxView->setCurrentIndex(index);

        QMenu menu{this};
        if (mailboxId.isEmpty())
        {
            auto* newMailboxAction = menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")),
                                                    i18n("New Mailbox…"));
            const auto accountResult = m_accountReader.findById(accountId.toStdString());
            const auto* account =
                std::get_if<std::optional<javelin::jmap::cache::CachedAccount>>(&accountResult);
            const bool canCreate = account != nullptr && account->has_value() &&
                                   !(*account)->isReadOnly && (*account)->hasMailCapability &&
                                   (*account)->mayCreateTopLevelMailbox;
            newMailboxAction->setEnabled(canCreate);
            if (!canCreate)
            {
                newMailboxAction->setToolTip(i18n(
                    "The server does not allow creating top-level mailboxes in this account."));
            }
            connect(newMailboxAction, &QAction::triggered, this,
                    [this, accountId]
                    {
                        bool accepted = false;
                        auto name =
                            QInputDialog::getText(this, i18n("New Mailbox"), i18n("Mailbox name:"),
                                                  QLineEdit::Normal, {}, &accepted)
                                .trimmed();
                        if (!accepted || name.isEmpty())
                            return;
                        auto task = m_mailCommandPort.createMailbox(accountId.toStdString(),
                                                                    name.toStdString());
                        QCoro::connect(
                            std::move(task), this,
                            [this](javelin::jmap::MailboxCreateResult result)
                            {
                                if (const auto* error =
                                        std::get_if<javelin::jmap::OperationError>(&result))
                                {
                                    presentError(*error);
                                    return;
                                }
                                const auto& created =
                                    std::get<javelin::jmap::MailboxCreateChange>(result);
                                m_statusBar->showMessage(i18n("Mailbox “%1” created.",
                                                              QString::fromStdString(created.name)),
                                                         3000);
                            });
                    });
            menu.addSeparator();
            auto* refreshAccountAction = menu.addAction(i18n("Refresh Account"));
            connect(refreshAccountAction, &QAction::triggered, this,
                    [this, account = accountId.toStdString()]
                    { refreshAccountFromServer(account); });
            auto* exportAccountAction = menu.addAction(
                QIcon::fromTheme(QStringLiteral("document-export")), i18n("Export Account…"));
            connect(exportAccountAction, &QAction::triggered, this, [this, accountId]
                    { m_mailExportController->exportAccount(accountId.toStdString()); });
            menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
            return;
        }
        const auto mailboxResult = m_mailboxReader.listMailboxTree(accountId.toStdString());
        if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&mailboxResult))
        {
            m_statusBar->showMessage(error->message, 10000);
            return;
        }
        const auto& currentMailboxes =
            std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(mailboxResult);
        const auto currentMailbox = std::ranges::find(currentMailboxes, mailboxId.toStdString(),
                                                      &javelin::jmap::cache::MailboxTreeItem::id);
        if (currentMailbox == currentMailboxes.end())
        {
            m_statusBar->showMessage(i18n("The mailbox is no longer available."), 5000);
            return;
        }
        const bool subscribed = currentMailbox->isSubscribed;

        auto* openAsTabAction = menu.addAction(i18n("Open as Tab"));
        connect(openAsTabAction, &QAction::triggered, this,
                [this] { openMailboxSelectionInTab(true); });
        auto* visibilityAction = menu.addAction(subscribed ? i18n("Hide") : i18n("Show"));
        connect(
            visibilityAction, &QAction::triggered, this,
            [this, accountId, mailboxId, subscribed]
            {
                auto task = m_mailCommandPort.setMailboxSubscribed(
                    accountId.toStdString(), mailboxId.toStdString(), !subscribed);
                QCoro::connect(
                    std::move(task), this,
                    [this, accountId, mailboxId,
                     subscribed](javelin::jmap::MailboxSubscriptionChangeResult result)
                    {
                        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        {
                            presentError(*error);
                            return;
                        }
                        if (subscribed)
                        {
                            const auto& snapshot = m_settings.snapshot();
                            javelin::protocol::SettingsUpdate update;
                            update.syncedMailboxSelections = withoutMailboxSelection(
                                snapshot.syncedMailboxSelections, accountId, mailboxId);
                            update.notificationMailboxSelections = withoutMailboxSelection(
                                snapshot.notificationMailboxSelections, accountId, mailboxId);
                            if (const auto error =
                                    m_settings.update(snapshot.revision, std::move(update)))
                            {
                                m_statusBar->showMessage(
                                    i18n("Mailbox hidden, but its local background settings could "
                                         "not be updated: %1",
                                         error->detail),
                                    10000);
                                return;
                            }
                        }
                        m_statusBar->showMessage(
                            subscribed ? i18n("Mailbox hidden.") : i18n("Mailbox shown."), 3000);
                    });
            });
        menu.addSeparator();
        auto* exportMailboxAction = menu.addAction(
            QIcon::fromTheme(QStringLiteral("document-export")), i18n("Export Mailbox…"));
        connect(exportMailboxAction, &QAction::triggered, this,
                [this, accountId, mailboxId]
                {
                    m_mailExportController->exportMailbox(accountId.toStdString(),
                                                          mailboxId.toStdString());
                });
        auto* propertiesAction = menu.addAction(i18n("Properties…"));
        connect(
            propertiesAction, &QAction::triggered, this,
            [this, accountId, mailboxId]
            {
                const auto result = m_mailboxReader.listMailboxTree(accountId.toStdString());
                if (const auto* error = std::get_if<javelin::jmap::cache::DatabaseError>(&result))
                {
                    m_statusBar->showMessage(error->message, 10000);
                    return;
                }

                const auto& mailboxes =
                    std::get<std::vector<javelin::jmap::cache::MailboxTreeItem>>(result);
                const auto mailbox = std::ranges::find(mailboxes, mailboxId.toStdString(),
                                                       &javelin::jmap::cache::MailboxTreeItem::id);
                if (mailbox == mailboxes.cend())
                {
                    m_statusBar->showMessage(i18n("The mailbox is no longer available."), 5000);
                    return;
                }

                auto accountName = m_settings.accountForCachedId(accountId).displayName;
                if (accountName.isEmpty())
                {
                    const auto accountResult = m_accountReader.findById(accountId.toStdString());
                    if (const auto* account =
                            std::get_if<std::optional<javelin::jmap::cache::CachedAccount>>(
                                &accountResult);
                        account != nullptr && account->has_value())
                        accountName = QString::fromStdString((*account)->name);
                }
                if (accountName.isEmpty())
                    accountName = i18n("Unnamed account");

                QString parentName;
                if (mailbox->parentId.has_value())
                {
                    const auto parentMailbox = std::ranges::find(
                        mailboxes, *mailbox->parentId, &javelin::jmap::cache::MailboxTreeItem::id);
                    parentName = parentMailbox == mailboxes.cend()
                                     ? i18n("Unavailable")
                                     : QString::fromStdString(parentMailbox->name);
                }

                const bool availableOffline =
                    m_settings.syncedMailboxIds(accountId).contains(mailboxId);
                const bool notificationsEnabled =
                    m_settings.notificationMailboxIds(accountId).contains(mailboxId);
                javelin::gui::mailboxes::MailboxPropertiesDialog dialog{
                    std::move(accountName), std::move(parentName), *mailbox,
                    availableOffline,       notificationsEnabled,  this};
                dialog.exec();
                if (!dialog.deleteRequested())
                    return;

                auto task = m_mailCommandPort.destroyMailbox(accountId.toStdString(),
                                                             mailboxId.toStdString());
                QCoro::connect(
                    std::move(task), this,
                    [this, accountId, mailboxId](javelin::jmap::MailboxDestroyResult destroyResult)
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::OperationError>(&destroyResult))
                        {
                            presentError(*error);
                            return;
                        }

                        const auto& snapshot = m_settings.snapshot();
                        javelin::protocol::SettingsUpdate update;
                        update.syncedMailboxSelections = withoutMailboxSelection(
                            snapshot.syncedMailboxSelections, accountId, mailboxId);
                        update.notificationMailboxSelections = withoutMailboxSelection(
                            snapshot.notificationMailboxSelections, accountId, mailboxId);
                        if (const auto error =
                                m_settings.update(snapshot.revision, std::move(update)))
                        {
                            m_statusBar->showMessage(
                                i18n("Mailbox deleted, but its local background settings could not "
                                     "be updated: %1",
                                     error->detail),
                                10000);
                            return;
                        }
                        m_statusBar->showMessage(i18n("Mailbox deleted."), 3000);
                    });
            });
        menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
    }

    void MainWindow::refreshMessageListPreservingSelection()
    {
        m_messageSelectionController->syncTabSelection(activeTab());

        QSignalBlocker blocker{m_messageView->selectionModel()};
        loadActiveTabFromCache(true, false);
    }

    void MainWindow::viewSelectedMessageSource()
    {
        const auto accountId = activeAccountId();
        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(i18n("Select a message to view its source."), 3000);
            return;
        }

        m_messageFileController->viewMessageSource(*accountId, *emailId);
    }

    void MainWindow::restorePersistentState()
    {
        auto state = deserializeMainWindowState(m_settings.workspaceSettings().mainWindowState,
                                                m_emailListSort);
        if (!state.geometry.isEmpty())
            restoreGeometry(state.geometry);
        if (!state.splitterState.isEmpty())
            m_mainSplitter->restoreState(state.splitterState);

        m_mailWorkspaceController->setSort(state.emailListSort);
        updateSortButton();

        m_mailWorkspaceController->prepareRestore(state.tabs.size());
        std::vector<std::optional<int>> restoredTabIndices;
        restoredTabIndices.reserve(state.tabs.size());
        for (auto& tab : state.tabs)
        {
            const auto tabCountBeforeRestore = m_tabs.size();
            std::visit(
                [this](auto& persisted)
                {
                    using Persisted = std::decay_t<decltype(persisted)>;
                    if constexpr (std::is_same_v<Persisted, PersistedMailboxTab>)
                        restoreMailboxTab(persisted);
                    else if constexpr (std::is_same_v<Persisted, PersistedSearchTab>)
                        restoreSearchTab(std::move(persisted));
                    else if constexpr (std::is_same_v<Persisted, PersistedComposeTab>)
                        restoreComposeTab(persisted);
                    else if constexpr (std::is_same_v<Persisted, PersistedContactsTab>)
                        restoreContactsTab(persisted);
                    else if constexpr (std::is_same_v<Persisted, PersistedCalendarTab>)
                        m_calendarTabController->open(persisted.displayedMonth);
                },
                tab);
            restoredTabIndices.push_back(
                m_tabs.size() > tabCountBeforeRestore
                    ? std::optional<int>{static_cast<int>(tabCountBeforeRestore)}
                    : std::nullopt);
        }

        if (m_tabs.empty())
        {
            for (int rootRow = 0; rootRow < m_mailboxModel->rowCount(); ++rootRow)
            {
                const auto rootIndex = m_mailboxModel->index(rootRow, 0);
                if (!rootIndex.isValid() || m_mailboxModel->rowCount(rootIndex) <= 0)
                {
                    continue;
                }

                const auto mailboxIndex = m_mailboxModel->index(0, 0, rootIndex);
                if (!mailboxIndex.isValid())
                {
                    continue;
                }

                m_mailboxView->setCurrentIndex(mailboxIndex);
                activateMailboxSelection(true);
                break;
            }
            return;
        }

        m_mailWorkspaceController->setActiveIndex(
            resolveRestoredActiveTabIndex(state.activeTabIndex, restoredTabIndices).value_or(0));
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
    }

    void MainWindow::restoreMailboxTab(const PersistedMailboxTab& tab)
    {
        auto plan = planMailboxTabRestore(tab);
        const auto index = m_mailWorkspaceController->restoreMailbox({
            .accountId = std::move(plan.accountId),
            .mailboxId = std::move(plan.mailboxId),
            .title = std::move(plan.title),
            .role = std::move(plan.mailboxRole),
            .sort = m_emailListSort,
            .restored = std::move(plan.restored),
        });
        tabSelection(m_tabs[static_cast<std::size_t>(index)]) = std::move(plan.selection);
    }

    void MainWindow::restoreSearchTab(PersistedSearchTab tab)
    {
        auto plan = planSearchTabRestore(std::move(tab));
        const auto index = m_mailWorkspaceController->restoreSearch({
            .accountId = std::move(plan.accountId),
            .criteria = std::move(plan.criteria),
            .sort = m_emailListSort,
            .restored = std::move(plan.restored),
        });
        tabSelection(m_tabs[static_cast<std::size_t>(index)]) = std::move(plan.selection);
    }

    void MainWindow::restoreComposeTab(const PersistedComposeTab& tab)
    {
        static_cast<void>(m_composeTabController->restore(tab));
    }

    void MainWindow::restoreContactsTab(const PersistedContactsTab& tab)
    {
        static_cast<void>(m_contactsTabController->restore(tab));
    }

    void MainWindow::savePersistentState() const
    {
        PersistedMainWindowState state{
            .geometry = saveGeometry(),
            .splitterState = m_mainSplitter->saveState(),
            .activeTabIndex = m_activeTabIndex.value_or(0),
            .emailListSort = m_emailListSort,
            .tabs = {},
        };
        state.tabs.reserve(m_tabs.size());
        for (const auto& tab : m_tabs)
            state.tabs.push_back(persistTab(tab));
        auto workspace = m_settings.workspaceSettings();
        workspace.mainWindowState = serializeMainWindowState(state);
        if (const auto error = m_settings.updateWorkspace(std::move(workspace)))
            qWarning().noquote() << QStringLiteral("Could not save the main-window state:")
                                 << error->detail;
    }

    void MainWindow::closeEvent(QCloseEvent* event)
    {
        savePersistentState();
        KXmlGuiWindow::closeEvent(event);
    }

} // namespace javelin::gui::shell
