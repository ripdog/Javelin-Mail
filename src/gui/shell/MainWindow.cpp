#include "gui/shell/MainWindow.h"

#include "app/AccountApplicationPorts.h"
#include "app/AccountRefreshApplicationPorts.h"
#include "app/ComposeApplicationPorts.h"
#include "app/MailApplicationEventsPorts.h"
#include "app/MailboxSession.h"
#include "app/MessageContentApplicationPorts.h"
#include "app/MessageListSession.h"
#include "app/MessageListSessionFactory.h"
#include "app/MessageNavigationPort.h"
#include "app/PerformanceMetrics.h"
#include "app/SearchSession.h"
#include "app/UndoApplicationPorts.h"
#include "gui/IconUtils.h"
#include "gui/logging/LogViewerDialog.h"
#include "gui/mailboxes/MailboxIconUtils.h"
#include "gui/mailboxes/MailboxPropertiesDialog.h"
#include "gui/mailboxes/MailboxSelection.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/mailboxes/MailboxTreeView.h"
#include "gui/messages/MessageDragListView.h"
#include "gui/messages/MessageListDelegate.h"
#include "gui/messages/MessageListModel.h"
#include "gui/messages/MessageListPanePresenter.h"
#include "gui/messageview/MessageViewContainer.h"
#include "gui/onboarding/FirstRunWizard.h"
#include "gui/search/AdvancedSearchDialog.h"
#include "gui/settings/ConnectionSettingsAdapter.h"
#include "gui/settings/GuiSettings.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/AccountRefreshController.h"
#include "gui/shell/CalendarTabController.h"
#include "gui/shell/ComposeTabController.h"
#include "gui/shell/ComposeTabPolicy.h"
#include "gui/shell/ContactsTabController.h"
#include "gui/shell/ElidingLabel.h"
#include "gui/shell/FocusedCommandRouter.h"
#include "gui/shell/LayeredStatusBar.h"
#include "gui/shell/MainWindowStateStore.h"
#include "gui/shell/MessageActionPolicy.h"
#include "gui/shell/MessageCommandController.h"
#include "gui/shell/MessageContentController.h"
#include "gui/shell/MessageContentPolicy.h"
#include "gui/shell/MessageFileController.h"
#include "gui/shell/MessageListTabBindingPresenter.h"
#include "gui/shell/MessageListTabController.h"
#include "gui/shell/MessageListTabPresenter.h"
#include "gui/shell/MessageNavigationController.h"
#include "gui/shell/MessageSelectionController.h"
#include "gui/shell/TabActivationPolicy.h"
#include "gui/shell/TabBarPresenter.h"
#include "gui/shell/TabPersistence.h"
#include "gui/sieve/SieveEditorDialog.h"
#include "gui/translation/TranslationService.h"
#include "jmap/cache/AccountReadRepository.h"
#include "jmap/cache/ContactReader.h"
#include "jmap/cache/IdentityReader.h"
#include "jmap/cache/MailboxReadRepository.h"
#include "jmap/cache/MessageViewReader.h"
#include "jmap/cache/QueryReader.h"
#include "jmap/calendar/CalendarReader.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactResults.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KLocalizedString>
#include <KStandardAction>

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
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleHints>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>

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
                           javelin::jmap::cache::ContactReader& contactReader,
                           javelin::jmap::calendar::CalendarReader& calendarReader,
                           javelin::app::CalendarCommandPort& calendarCommandPort,
                           javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
                           javelin::jmap::cache::IdentityReader& identityReader,
                           javelin::jmap::cache::MessageViewReader& messageViewReader,
                           javelin::jmap::cache::QueryReader& queryReader,
                           javelin::gui::translation::TranslationService& translationService,
                           javelin::app::ComposeCommandPort& composeCommandPort,
                           javelin::app::ContactCommandPort& contactCommandPort,
                           javelin::app::MailCommandPort& mailCommandPort,
                           javelin::app::SieveCommandPort& sieveCommandPort,
                           javelin::app::AccountRefreshPort& accountRefreshPort,
                           javelin::app::OnboardingPort& onboardingPort,
                           javelin::app::MessageContentPort& messageContentPort,
                           javelin::app::MessageListSessionFactoryPort& messageListSessionFactory,
                           javelin::app::MailApplicationEventsPort& mailEvents,
                           javelin::app::MessageNavigationPort& messageNavigationPort,
                           javelin::app::UndoCommandPort& undoCommandPort, QWidget* parent)
        : KXmlGuiWindow(parent), m_settings(settings), m_accountCommandPort(accountCommandPort),
          m_accountReader(accountReader), m_mailboxReader(mailboxReader),
          m_contactReader(contactReader), m_calendarReader(calendarReader),
          m_calendarCommandPort(calendarCommandPort),
          m_contactIdentityLookup(contactIdentityLookup), m_identityReader(identityReader),
          m_messageViewReader(messageViewReader), m_queryReader(queryReader),
          m_translationService(translationService), m_composeCommandPort(composeCommandPort),
          m_contactCommandPort(contactCommandPort), m_mailCommandPort(mailCommandPort),
          m_sieveCommandPort(sieveCommandPort), m_accountRefreshPort(accountRefreshPort),
          m_onboardingPort(onboardingPort), m_messageContentPort(messageContentPort),
          m_messageListSessionFactory(messageListSessionFactory), m_mailEvents(mailEvents),
          m_messageNavigationPort(messageNavigationPort), m_undoCommandPort(undoCommandPort)
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
        setupUi();
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this] { scheduleApplicationPaletteRefresh(); });
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
                m_statusBar->showMessage(
                    completed->stack == javelin::app::undo::HistoryStack::Redo
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
        m_accountRefreshController =
            new AccountRefreshController(m_settings, m_accountRefreshPort, m_accountReader, this);
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
                    m_statusBar->showMessage(
                        i18n("Synced %1 mailboxes and %2 messages for %3.", summary.mailboxCount,
                             summary.emailCount, accountName),
                        10000);
                });
        connect(m_accountRefreshController, &AccountRefreshController::contactsRefreshed, this,
                [this](const javelin::jmap::contacts::ContactRefreshSummary&)
                { reloadAccounts(); });
        m_calendarTabController = new CalendarTabController(
            m_settings, m_calendarReader, m_calendarCommandPort, *m_contentStack, m_tabs, this);
        connect(m_calendarTabController, &CalendarTabController::tabReady, this,
                [this](const int index)
                {
                    m_activeTabIndex = index;
                    updateTabBar();
                    activateTab(index, false);
                });
        connect(m_calendarTabController, &CalendarTabController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_calendarTabController, &CalendarTabController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        m_contactsTabController =
            new ContactsTabController(m_settings, m_contactReader, m_accountRefreshPort,
                                      m_contactCommandPort, *m_contentStack, m_tabs, this);
        connect(m_contactsTabController, &ContactsTabController::tabReady, this,
                [this](const int index)
                {
                    m_activeTabIndex = index;
                    updateTabBar();
                    activateTab(index, false);
                });
        connect(m_contactsTabController, &ContactsTabController::toolbarStateChanged, this,
                &MainWindow::updateToolbarForActiveTab);
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
        m_composeTabController =
            new ComposeTabController(m_settings, m_composeCommandPort, m_identityReader,
                                     m_contactIdentityLookup, *m_contentStack, m_tabs, this);
        connect(m_composeTabController, &ComposeTabController::tabReady, this,
                [this](const int index)
                {
                    m_activeTabIndex = index;
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
        m_messageListTabBindingPresenter = std::make_unique<MessageListTabBindingPresenter>(
            *m_mailboxModel, *m_mailboxView, *m_mailboxSearchEdit, *m_messageModel, *m_mailboxPane);
        m_messageSelectionController = std::make_unique<MessageSelectionController>(
            *m_mailboxModel, *m_mailboxView, *m_messageModel, *m_messageView);
        m_messageListTabController = new MessageListTabController(
            m_queryReader, m_messageListSessionFactory, pageSize, this, this);
        m_messageNavigationController = std::make_unique<MessageNavigationController>(
            m_messageNavigationPort, *m_messageListTabController);
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
                    markTabsStaleForAccount(unavailable.accountId);
                    refreshActiveTabFromServer();
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
                    updateMessageActions();
                    if (!summary.usedCachedContent)
                        m_statusBar->showMessage(i18n("Message ready."), 5000);
                });
        connect(m_messageListTabController, &MessageListTabController::pageChanged, this,
                [this](javelin::app::MessageListSession* session)
                {
                    const auto* tab = activeTab();
                    if (tab == nullptr || !m_messageListTabController->ownsSession(*tab, session))
                        return;

                    applyActiveTabPagePreservingSelection(
                        m_messageSelectionController->currentRow());
                    updateEmptyStates();
                    updateMessageListHeader();
                    resolveOpenEmailRoute();
                });
        connect(m_messageListTabController, &MessageListTabController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(&m_mailEvents, &javelin::app::MailApplicationEventsPort::sessionCapabilitiesChanged,
                this, [this](const QString&) { reloadAccounts(); });
        createActions();
        setupGUI(KXmlGuiWindow::ToolBar | KXmlGuiWindow::Keys | KXmlGuiWindow::Save |
                     KXmlGuiWindow::Create,
                 QStringLiteral("javelinmailui.rc"));
        updateToolbarForActiveTab();
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
            m_mailboxModel->setConnectionStatus(accountId, modelStatus);
            updateAuthenticationPrompt(accountId, status);
        };
        connect(&m_mailEvents, &javelin::app::MailApplicationEventsPort::accountStatusChanged, this,
                applyAccountStatus);
        for (const auto& [accountId, status] : m_mailEvents.accountStatuses())
            applyAccountStatus(QString::fromStdString(accountId), status);
        connect(&m_mailEvents, &javelin::app::MailApplicationEventsPort::cacheInvalidated, this,
                [this](const javelin::app::MailCacheInvalidation& invalidation)
                {
                    const auto& change = invalidation.change;
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

    MainWindow::~MainWindow() = default;

    void MainWindow::openEmailRoute(const javelin::app::OpenEmailRoute& route)
    {
        const auto accountId = QString::fromStdString(route.accountId);
        const auto mailboxId = QString::fromStdString(route.mailboxId);

        reloadAccounts();
        QString mailboxTitle = mailboxId;
        if (route.mailboxName.has_value() && !route.mailboxName->empty())
            mailboxTitle = QString::fromStdString(*route.mailboxName);
        std::optional<std::string> mailboxRole;
        std::optional<std::size_t> totalThreads;
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
            const auto totalThreadsValue =
                mailboxIndex.data(javelin::gui::mailboxes::MailboxTreeModel::TotalThreadsRole);
            if (totalThreadsValue.isValid())
                totalThreads = static_cast<std::size_t>(totalThreadsValue.toULongLong());
        }

        activateMailboxInHomeTab(route.accountId, route.mailboxId, mailboxTitle, mailboxRole,
                                 totalThreads, false);
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

        m_messageViewContainer->setSelection(m_messageViewReader, route.accountId, route.mailboxId,
                                             route.emailId);
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

        m_contactsAction = new QAction(QIcon::fromTheme(QStringLiteral("view-pim-contacts")),
                                       i18n("Contacts"), this);
        connect(m_contactsAction, &QAction::triggered, this, &MainWindow::openContacts);
        actionCollection()->addAction(QStringLiteral("open_contacts"), m_contactsAction);
        m_contactsAction->setEnabled(true);

        m_calendarAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-month")),
                                       i18n("Calendar"), this);
        connect(m_calendarAction, &QAction::triggered, this, &MainWindow::openCalendar);
        actionCollection()->addAction(QStringLiteral("open_calendar"), m_calendarAction);
        m_calendarAction->setEnabled(true);

        m_sieveAction = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                    i18n("Sieve Rules"), this);
        connect(m_sieveAction, &QAction::triggered, this, &MainWindow::openSieveEditor);
        actionCollection()->addAction(QStringLiteral("open_sieve_editor"), m_sieveAction);

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
        connect(m_archiveAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->archiveSelection(
                        activeAccountId(), activeMailboxId(), activeTabIsSearch());
                });
        actionCollection()->addAction(QStringLiteral("archive_email"), m_archiveAction);
        actionCollection()->setDefaultShortcut(m_archiveAction, QKeySequence{Qt::Key_A});

        m_markUnreadAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/unread.svg")),
                        i18nc("@action", "Mark &Unread"), this);
        connect(m_markUnreadAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->markSelectionUnread(activeAccountId(),
                                                                    activeMailboxId());
                });
        actionCollection()->addAction(QStringLiteral("mark_email_unread"), m_markUnreadAction);

        m_deleteAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/delete.svg")),
                        i18nc("@action", "&Delete"), this);
        m_deleteAction->setShortcut(QKeySequence::Delete);
        connect(
            m_deleteAction, &QAction::triggered, this, [this]
            { m_messageCommandController->deleteSelection(activeAccountId(), activeMailboxId()); });
        actionCollection()->addAction(QStringLiteral("delete_email"), m_deleteAction);
        actionCollection()->setDefaultShortcut(m_deleteAction, QKeySequence::Delete);

        m_permanentDeleteAction = new QAction(i18n("Delete Permanently"), this);
        connect(m_permanentDeleteAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->permanentlyDeleteSelection(activeAccountId(),
                                                                           activeMailboxId());
                });
        actionCollection()->addAction(QStringLiteral("permanently_delete_email"),
                                      m_permanentDeleteAction);
        actionCollection()->setDefaultShortcut(m_permanentDeleteAction,
                                               QKeySequence{Qt::SHIFT | Qt::Key_Delete});

        m_moveAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-move")),
                                   i18nc("@action", "&Move to…"), this);
        connect(m_moveAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->showTransferMenu(
                        MessageTransferOperation::Move, activeAccountId(), activeMailboxId(),
                        activeTabIsSearch());
                });
        actionCollection()->addAction(QStringLiteral("move_email"), m_moveAction);

        m_copyAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                   i18nc("@action", "&Copy to…"), this);
        connect(m_copyAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->showTransferMenu(
                        MessageTransferOperation::Copy, activeAccountId(), activeMailboxId(),
                        activeTabIsSearch());
                });
        actionCollection()->addAction(QStringLiteral("copy_email"), m_copyAction);

        m_viewSourceAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                         i18nc("@action", "View &Source"), this);
        connect(m_viewSourceAction, &QAction::triggered, this,
                &MainWindow::viewSelectedMessageSource);
        actionCollection()->addAction(QStringLiteral("view_message_source"), m_viewSourceAction);

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

        m_composeSaveDraftAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                               i18n("Save Draft"), this);
        connect(m_composeSaveDraftAction, &QAction::triggered, this,
                [this] { m_composeTabController->saveDraft(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_save_draft"),
                                      m_composeSaveDraftAction);

        m_composeAttachFilesAction =
            new QAction(QIcon::fromTheme(QStringLiteral("mail-attachment")),
                        i18n("Attach Files"), this);
        connect(m_composeAttachFilesAction, &QAction::triggered, this,
                [this] { m_composeTabController->attachFiles(activeTab()); });
        actionCollection()->addAction(QStringLiteral("compose_attach_files"),
                                      m_composeAttachFilesAction);

        const auto invokeContact = [this](const ContactsTabCommand command)
        { m_contactsTabController->invoke(activeTab(), command); };
        m_contactNewAction = new QAction(QIcon::fromTheme(QStringLiteral("contact-new")),
                                         i18nc("@action", "Add"), this);
        connect(m_contactNewAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::CreateContact); });
        auto* contactAddMenu = new QMenu(this);
        auto* newContact = contactAddMenu->addAction(
            QIcon::fromTheme(QStringLiteral("contact-new")), i18n("New Contact"));
        connect(newContact, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::CreateContact); });
        auto* newGroup = contactAddMenu->addAction(QIcon::fromTheme(QStringLiteral("system-users")),
                                                   i18n("New Group"));
        connect(newGroup, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::CreateGroup); });
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
        m_contactCopyAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                          i18n("Copy Contact"), this);
        connect(m_contactCopyAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::CopyContact); });
        actionCollection()->addAction(QStringLiteral("contact_copy"), m_contactCopyAction);
        m_contactImportAction = new QAction(QIcon::fromTheme(QStringLiteral("document-import")),
                                            i18n("Import vCard…"), this);
        connect(m_contactImportAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::ImportVCard); });
        actionCollection()->addAction(QStringLiteral("contact_import"), m_contactImportAction);
        m_contactExportAction = new QAction(QIcon::fromTheme(QStringLiteral("document-export")),
                                            i18n("Export vCard…"), this);
        connect(m_contactExportAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::ExportVCard); });
        actionCollection()->addAction(QStringLiteral("contact_export"), m_contactExportAction);
        m_contactDuplicatesAction = new QAction(QIcon::fromTheme(QStringLiteral("merge")),
                                                i18n("Find Duplicates…"), this);
        connect(m_contactDuplicatesAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::FindDuplicates); });
        actionCollection()->addAction(QStringLiteral("contact_duplicates"),
                                      m_contactDuplicatesAction);
        m_contactAddToGroupAction = new QAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                                i18n("Add to Group"), this);
        auto* addToGroupMenu = new QMenu(this);
        connect(addToGroupMenu, &QMenu::aboutToShow, this, [this, addToGroupMenu]
                { m_contactsTabController->populateAddToGroupMenu(activeTab(), *addToGroupMenu); });
        m_contactAddToGroupAction->setMenu(addToGroupMenu);
        actionCollection()->addAction(QStringLiteral("contact_add_to_group"),
                                      m_contactAddToGroupAction);
        m_contactRemoveFromGroupAction =
            new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), i18n("Remove from Group"),
                        this);
        auto* removeFromGroupMenu = new QMenu(this);
        connect(removeFromGroupMenu, &QMenu::aboutToShow, this,
                [this, removeFromGroupMenu]
                {
                    m_contactsTabController->populateRemoveFromGroupMenu(activeTab(),
                                                                         *removeFromGroupMenu);
                });
        m_contactRemoveFromGroupAction->setMenu(removeFromGroupMenu);
        actionCollection()->addAction(QStringLiteral("contact_remove_from_group"),
                                      m_contactRemoveFromGroupAction);
        m_contactManageAddressBooksAction =
            new QAction(QIcon::fromTheme(QStringLiteral("view-list-details")),
                        i18n("Manage Address Books…"), this);
        connect(m_contactManageAddressBooksAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::ManageAddressBooks); });
        actionCollection()->addAction(QStringLiteral("contact_manage_address_books"),
                                      m_contactManageAddressBooksAction);
        m_contactRefreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                             i18n("Refresh Contacts"), this);
        connect(m_contactRefreshAction, &QAction::triggered, this,
                [invokeContact] { invokeContact(ContactsTabCommand::Refresh); });
        actionCollection()->addAction(QStringLiteral("contact_refresh"), m_contactRefreshAction);

        const auto invokeCalendar = [this](const CalendarTabCommand command)
        { m_calendarTabController->invoke(activeTab(), command); };
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
        m_calendarTodayAction = new QAction(QIcon::fromTheme(QStringLiteral("go-jump-today")),
                                            i18n("Today"), this);
        connect(m_calendarTodayAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::Today); });
        actionCollection()->addAction(QStringLiteral("calendar_today"), m_calendarTodayAction);
        m_calendarNextMonthAction = new QAction(QIcon::fromTheme(QStringLiteral("go-next")),
                                                i18n("Next Month"), this);
        connect(m_calendarNextMonthAction, &QAction::triggered, this,
                [invokeCalendar] { invokeCalendar(CalendarTabCommand::NextMonth); });
        actionCollection()->addAction(QStringLiteral("calendar_next_month"),
                                      m_calendarNextMonthAction);
        m_calendarListAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-list")),
                                           i18n("Calendars"), this);
        actionCollection()->addAction(QStringLiteral("calendar_list"), m_calendarListAction);
        m_calendarRefreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                              i18n("Refresh Calendar"), this);
        connect(m_calendarRefreshAction, &QAction::triggered, this,
                [this] { refreshActiveTabFromServer(); });
        actionCollection()->addAction(QStringLiteral("calendar_refresh"), m_calendarRefreshAction);

        auto* logAction = new QAction(QIcon::fromTheme(QStringLiteral("view-list-text")),
                                      i18n("Application Log"), this);
        connect(logAction, &QAction::triggered, this,
                [this]
                {
                    auto* dialog = new javelin::gui::logging::LogViewerDialog(this);
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                });
        actionCollection()->addAction(QStringLiteral("open_application_log"), logAction);
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
            removeButton = messageBox.addButton(i18n("Remove from History"),
                                                QMessageBox::DestructiveRole);
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
            m_accountReader, m_mailboxReader, m_queryReader.databasePath(),
            {.accountId = std::nullopt,
             .showAccount = true,
             .checkable = false,
             .checkedMailboxIds = {},
             .accountDisplayName = [this](const QStringView accountId)
             { return m_settings.accountForCachedId(accountId).displayName; }},
            this);
        m_messageModel = new javelin::gui::messages::MessageListModel(m_queryReader, this);

        m_mailboxSearchEdit = new QLineEdit(this);
        m_mailboxSearchEdit->setClearButtonEnabled(true);
        m_mailboxSearchEdit->setPlaceholderText(i18n("Search this account on the server"));

        m_tabBar = new QTabBar(this);
        m_tabBar->setDocumentMode(true);
        m_tabBar->setTabsClosable(true);
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
            m_mailCommandPort, m_mailboxReader, *m_messageView, this, this);
        connect(m_messageCommandController, &MessageCommandController::statusMessage, this,
                [this](const QString& message, const int durationMilliseconds)
                { m_statusBar->showMessage(message, durationMilliseconds); });
        connect(m_messageCommandController, &MessageCommandController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(m_messageCommandController, &MessageCommandController::mailboxMembershipChanged,
                this,
                [this](const QString& accountId)
                {
                    markTabsStaleForAccount(accountId.toStdString());
                    refreshMessageListPreservingSelection();
                    refreshSelectionFromModels();
                    updateEmptyStates();
                    updateMessageListHeader();
                });
        connect(m_messageCommandController, &MessageCommandController::messageMetadataChanged, this,
                [this](const QString& accountId)
                {
                    markSearchTabsStaleForAccount(accountId.toStdString());
                    refreshMessageListPreservingSelection();
                    refreshSelectionFromModels();
                });
        connect(m_messageCommandController, &MessageCommandController::emailMarkedRead, this,
                [this](const QString& accountId, const QString& emailId)
                {
                    markSearchTabsStaleForAccount(accountId.toStdString());
                    static_cast<void>(m_messageModel->setEmailRead(emailId.toStdString()));
                    updateMessageActions();
                });
        connect(m_messageCommandController, &MessageCommandController::emailMutationsSubmitted,
                this,
                [this](const EmailMutationSubmissionSummary& summary)
                {
                    if (summary.failedEmailCount > 0)
                    {
                        markTabsStaleForAccount(summary.accountId);
                        refreshActiveTabFromServer();
                        presentError(javelin::jmap::OperationError{
                            .message = i18np(
                                "The server rejected %1 email mutation. The mailbox has been "
                                "refreshed to restore the server state.",
                                "The server rejected %1 email mutations. The mailbox has been "
                                "refreshed to restore the server state.",
                                summary.failedEmailCount),
                        });
                        return;
                    }
                    if (summary.updatedEmailCount == 0)
                        return;
                    markTabsStaleForAccount(summary.accountId);
                    refreshActiveTabFromServer();
                });

        connect(m_mailboxModel, &javelin::gui::mailboxes::MailboxTreeModel::emailsDropped, this,
                [this](const QString& sourceAccountId, const QString& destinationAccountId,
                       const QString& destinationMailboxId, const QStringList& emailIds)
                {
                    Q_UNUSED(emailIds);
                    const auto sourceAccount = activeAccountId();
                    const auto sourceMailboxId = activeMailboxId();
                    if (!sourceAccount.has_value() || !sourceMailboxId.has_value() ||
                        sourceAccountId.toStdString() != *sourceAccount ||
                        sourceAccountId != destinationAccountId)
                    {
                        m_statusBar->showMessage(
                            i18n("Messages can only be moved within their account."), 5000);
                        return;
                    }

                    auto selection = m_messageCommandController->selectedActionItems();
                    m_messageCommandController->queueTransfer(
                        sourceAccountId.toStdString(), *sourceMailboxId,
                        destinationMailboxId.toStdString(), std::move(selection),
                        MessageTransferOperation::Move, i18n("Queued move."));
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
        m_messageSortButton = new QToolButton(messageHeaderRow);
        m_messageSortButton->setIcon(javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/display-options.svg"),
            palette().color(QPalette::Text)));
        m_messageSortButton->setToolTip(i18n("Sort messages"));
        m_firstPageButton = new QToolButton(messageHeaderRow);
        m_firstPageButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
        m_firstPageButton->setToolTip(i18n("First page"));
        m_previousPageButton = new QToolButton(messageHeaderRow);
        m_previousPageButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/nav-left.svg"),
                                        palette().color(QPalette::Text)));
        m_previousPageButton->setToolTip(i18n("Previous page"));
        m_nextPageButton = new QToolButton(messageHeaderRow);
        m_nextPageButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/nav-right.svg"),
                                        palette().color(QPalette::Text)));
        m_nextPageButton->setToolTip(i18n("Next page"));
        m_lastPageButton = new QToolButton(messageHeaderRow);
        m_lastPageButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
        m_lastPageButton->setToolTip(i18n("Last page"));
        m_pageNumberSpinBox = new QSpinBox(messageHeaderRow);
        m_pageNumberSpinBox->setPrefix(i18nc("@item page number prefix", "Page "));
        m_pageNumberSpinBox->setAlignment(Qt::AlignCenter);
        m_pageNumberSpinBox->setToolTip(i18n("Enter a page number"));
        m_messagePageLabel = new QLabel(messageHeaderRow);
        auto titleFont = m_messageListTitleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_messageListTitleLabel->setFont(titleFont);
        messageHeaderRowLayout->addWidget(m_messageListTitleLabel, 1);
        messageHeaderRowLayout->addWidget(m_messageListMetaLabel);
        messageHeaderRowLayout->addWidget(m_searchServerButton);
        messageHeaderRowLayout->addWidget(m_firstPageButton);
        messageHeaderRowLayout->addWidget(m_previousPageButton);
        messageHeaderRowLayout->addWidget(m_pageNumberSpinBox);
        messageHeaderRowLayout->addWidget(m_messagePageLabel);
        messageHeaderRowLayout->addWidget(m_nextPageButton);
        messageHeaderRowLayout->addWidget(m_lastPageButton);
        messageHeaderRowLayout->addWidget(m_messageSortButton);
        messageHeaderLayout->addWidget(messageHeaderRow);
        m_messageLoadingIndicator = new QProgressBar(messageHeader);
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
        m_messageEmptyState = new QLabel(i18n("No messages"), messageListArea);
        m_messageEmptyState->setAlignment(Qt::AlignCenter);
        m_messageEmptyState->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_messageEmptyState->setWordWrap(true);
        m_messageEmptyState->setAttribute(Qt::WA_TransparentForMouseEvents);
        messageListAreaLayout->addWidget(m_messageView, 0, 0);
        messageListAreaLayout->addWidget(m_messageEmptyState, 0, 0);
        messageLayout->addWidget(messageHeader);
        messageLayout->addWidget(messageListArea, 1);

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
                &MainWindow::showMessageListContextMenu);
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
        m_messageListPanePresenter =
            std::make_unique<javelin::gui::messages::MessageListPanePresenter>(
                *m_messageListTitleLabel, *m_messageListMetaLabel, *m_messagePageLabel,
                *m_messageEmptyState, *m_messageView, *m_messageLoadingIndicator,
                *m_searchServerButton, *m_firstPageButton, *m_previousPageButton,
                *m_pageNumberSpinBox, *m_nextPageButton, *m_lastPageButton, pageSize);
        m_messageListTabPresenter = std::make_unique<MessageListTabPresenter>(
            *m_messageListPanePresenter, *m_tabBarPresenter);
        updateEmptyStates();
        updateMessageListHeader();
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
        connect(m_tabBarPresenter, &TabBarPresenter::closeRequested, this, &MainWindow::closeTab);
        connect(m_firstPageButton, &QToolButton::clicked, this, &MainWindow::goToFirstPage);
        connect(m_previousPageButton, &QToolButton::clicked, this, &MainWindow::goToPreviousPage);
        connect(m_nextPageButton, &QToolButton::clicked, this, &MainWindow::goToNextPage);
        connect(m_lastPageButton, &QToolButton::clicked, this, &MainWindow::goToLastPage);
        connect(m_pageNumberSpinBox, &QSpinBox::editingFinished, this,
                [this]
                {
                    if (m_pageNumberSpinBox->value() > 0)
                    {
                        goToPage(static_cast<std::size_t>(m_pageNumberSpinBox->value() - 1));
                    }
                });
        connect(m_messageSortButton, &QToolButton::clicked, this, &MainWindow::showSortMenu);
        connect(m_searchServerButton, &QToolButton::clicked, this,
                [this]
                {
                    if (auto* tab = activeTab(); tab != nullptr)
                        static_cast<void>(m_messageListTabController->promoteSearch(*tab));
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
                { handleCurrentMessageChanged(current); });
        connect(m_messageView->selectionModel(), &QItemSelectionModel::selectionChanged, this,
                [this](const QItemSelection&, const QItemSelection&)
                { refreshSelectionFromModels(); });
    }

    void MainWindow::handleCurrentMessageChanged(const QModelIndex& current)
    {
        if (m_modelUpdateInProgress)
            return;
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        const bool allowSearchSelection = activeTabIsSearch() && accountId.has_value();
        if (!accountId.has_value() || (!mailboxId.has_value() && !allowSearchSelection))
        {
            m_messageViewContainer->setSelection(m_messageViewReader, accountId, mailboxId,
                                                 std::nullopt);
            updateMessageActions();
            return;
        }

        if (!current.isValid())
        {
            m_messageViewContainer->setSelection(m_messageViewReader, accountId, mailboxId,
                                                 std::nullopt);
            updateMessageActions();
            return;
        }

        const auto emailId =
            current.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
        const auto threadId =
            current.data(javelin::gui::messages::MessageListModel::ThreadIdRole).toString();
        const auto selectedEmailId = emailId.toStdString();
        const auto selectedThreadId = threadId.toStdString();
        m_messageNavigationController->cancelIfSelectionChanged(
            activeTab(), selectedEmailId, std::optional<std::string_view>{selectedThreadId});
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
        m_messageViewContainer->setSelection(
            m_messageViewReader, accountId, mailboxId,
            emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                              : std::optional<std::string>{emailId.toStdString()});
        updateEmptyStates();
        updateMessageListHeader();
        updateMessageActions();
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
        m_contactsTabController->open(activeAccountId());
    }

    void MainWindow::openSieveEditor()
    {
        const auto accountId = activeAccountId();
        if (!accountId)
        {
            m_statusBar->showMessage(i18n("Select an account to edit its Sieve rules."), 5000);
            return;
        }
        javelin::gui::sieve::SieveEditorDialog dialog{m_sieveCommandPort, *accountId, this};
        dialog.exec();
    }

    void MainWindow::openCalendar()
    {
        m_calendarTabController->open();
    }

    void MainWindow::composeNewMessage()
    {
        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("Select an account before composing a message."),
                                     5000);
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
        return activeWorkspaceTab(m_tabs, m_activeTabIndex);
    }

    TabState* MainWindow::activeTab()
    {
        return activeWorkspaceTab(m_tabs, m_activeTabIndex);
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

    bool MainWindow::activeTabIsCalendar() const
    {
        const auto* tab = activeTab();
        return tab != nullptr && tabKind(*tab) == TabKind::Calendar;
    }

    std::optional<std::string> MainWindow::activeAccountId() const
    {
        const auto* tab = activeTab();
        return tab == nullptr ? std::optional<std::string>{std::nullopt} : tabAccountId(*tab);
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

    void MainWindow::updateToolbarForActiveTab()
    {
        const auto context = toolbarContextForActiveTab();
        setToolBarVisible(QStringLiteral("mainToolBar"), context == ToolbarContext::Mail);
        setToolBarVisible(QStringLiteral("composeToolBar"), context == ToolbarContext::Compose);
        setToolBarVisible(QStringLiteral("contactsToolBar"), context == ToolbarContext::Contacts);
        setToolBarVisible(QStringLiteral("calendarToolBar"), context == ToolbarContext::Calendar);
        if (context == ToolbarContext::Contacts)
        {
            const auto state = m_contactsTabController->toolbarState(activeTab());
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
        }
        if (context == ToolbarContext::Calendar)
        {
            auto* menu = m_calendarTabController->calendarMenuForTab(activeTab());
            m_calendarListAction->setMenu(menu);
            m_calendarListAction->setEnabled(menu != nullptr);
        }
    }

    void MainWindow::saveNewToolbarConfig()
    {
        KXmlGuiWindow::saveNewToolbarConfig();
        updateToolbarForActiveTab();
    }

    void MainWindow::openOrActivateMailboxTab(std::string accountId, std::string mailboxId,
                                              const QString title, std::optional<std::string> role,
                                              const bool refreshRemote)
    {
        const auto result = m_messageListTabController->openOrCreateMailbox(
            m_tabs, {
                        .accountId = std::move(accountId),
                        .mailboxId = std::move(mailboxId),
                        .title = title,
                        .role = std::move(role),
                        .sort = m_emailListSort,
                        .restored = std::nullopt,
                    });
        m_activeTabIndex = static_cast<int>(result.index);
        updateTabBar();
        activateTab(*m_activeTabIndex, refreshRemote);
    }

    void MainWindow::activateMailboxInHomeTab(std::string accountId, std::string mailboxId,
                                              QString title, std::optional<std::string> role,
                                              const std::optional<std::size_t> total,
                                              const bool refreshRemote)
    {
        auto tab = m_messageListTabController->createMailboxTab({
            .accountId = std::move(accountId),
            .mailboxId = std::move(mailboxId),
            .title = std::move(title),
            .role = std::move(role),
            .sort = m_emailListSort,
            .restored =
                javelin::app::RestoredMailboxState{
                    .page =
                        javelin::app::MessageListPage{
                            .offset = 0,
                            .installedOffset = std::nullopt,
                            .pendingOffset = std::nullopt,
                            .position = 0,
                            .returnedLimit = pageSize,
                            .total = total,
                            .queryState = {},
                            .anchor = std::nullopt,
                            .items = {},
                            .cacheLoaded = false,
                            .refreshInFlight = false,
                            .stale = false,
                            .refreshError = {},
                        },
                },
        });
        if (m_tabs.empty())
        {
            m_tabs.push_back(std::move(tab));
        }
        else
        {
            m_messageListTabController->releaseSession(m_tabs[0]);
            m_tabs[0] = std::move(tab);
        }

        m_activeTabIndex = 0;
        updateTabBar();
        activateTab(0, refreshRemote);
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
        const auto result = m_messageListTabController->openOrCreateSearch(
            m_tabs, {
                        .accountId = std::move(accountId),
                        .criteria = std::move(criteria),
                        .sort = m_emailListSort,
                        .restored = std::nullopt,
                    });
        m_activeTabIndex = static_cast<int>(result.index);
        updateTabBar();
        activateTab(*m_activeTabIndex, refreshRemote);
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
            m_activeTabIndex.reset();
            m_messageModel->clear();
            m_messageViewContainer->setSelection(m_messageViewReader, std::nullopt, std::nullopt,
                                                 std::nullopt);
            if (m_contentStack != nullptr)
                m_contentStack->setCurrentIndex(0);
            if (m_mailboxPane != nullptr)
                m_mailboxPane->setVisible(plan.showMailboxPane);
            updateTabBar();
            updateEmptyStates();
            updateMessageListHeader();
            updateMessageActions();
            updateToolbarForActiveTab();
            metrics.finish(QStringLiteral("empty"));
            return;
        }

        m_activeTabIndex = index;
        auto& tab = m_tabs[static_cast<std::size_t>(index)];
        const auto initialPlan = planTabActivation({
            .kind = tabKind(tab),
            .homeTab = index == 0,
            .messagePageStale = m_messageListTabController->pageStale(tab),
            .remoteRefreshRequested = refreshRemote,
        });

        updateToolbarForActiveTab();
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
            .messagePageStale = m_messageListTabController->pageStale(tab),
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
                        .messagePageStale = m_messageListTabController->pageStale(currentTab),
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
        if (!tabCanClose(m_tabs[static_cast<std::size_t>(index)], static_cast<std::size_t>(index)))
        {
            return;
        }

        if (tabKind(m_tabs[static_cast<std::size_t>(index)]) == TabKind::Compose &&
            !closeComposeTab(index))
        {
            return;
        }

        if (tabKind(m_tabs[static_cast<std::size_t>(index)]) == TabKind::Contacts &&
            !m_contactsTabController->close(m_tabs[static_cast<std::size_t>(index)]))
        {
            return;
        }

        if (tabKind(m_tabs[static_cast<std::size_t>(index)]) == TabKind::Calendar)
            static_cast<void>(
                m_calendarTabController->close(m_tabs[static_cast<std::size_t>(index)]));

        m_messageListTabController->releaseSession(m_tabs[static_cast<std::size_t>(index)]);
        m_activeTabIndex = activeTabIndexAfterClose(m_tabs.size(), m_activeTabIndex, index);
        m_tabs.erase(m_tabs.begin() + index);
        if (!m_activeTabIndex.has_value())
        {
            activateTab(-1, false);
            return;
        }

        updateTabBar();
        activateTab(*m_activeTabIndex, false);
    }

    void
    MainWindow::applyActiveTabPagePreservingSelection(const std::optional<int> previousMessageRow)
    {
        bool autoSelectedFallback = false;
        const bool wasUpdatingModel = m_modelUpdateInProgress;
        m_modelUpdateInProgress = true;
        {
            QSignalBlocker messageSelectionBlocker{m_messageView->selectionModel()};
            m_messageListTabBindingPresenter->applyPage(activeTab());
            autoSelectedFallback =
                m_messageSelectionController->restoreTabSelection(activeTab(), previousMessageRow);
        }
        m_modelUpdateInProgress = wasUpdatingModel;
        if (autoSelectedFallback)
            handleCurrentMessageChanged(m_messageView->currentIndex());
        else
            refreshSelectionFromModels();
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
            m_messageListTabBindingPresenter->applyPage(nullptr);
            refreshSelectionFromModels();
            metrics.finish(QStringLiteral("empty"));
            return;
        }

        if (!m_messageListTabController->loadCachedPage(*tab, forceReload))
        {
            const auto plan = planTabActivation({
                .kind = tabKind(*tab),
                .homeTab = m_activeTabIndex == std::optional<int>{0},
            });
            if (plan.clearMessagePresentation)
            {
                m_messageModel->clear();
                m_messageViewContainer->setSelection(m_messageViewReader, std::nullopt,
                                                     std::nullopt, std::nullopt);
            }
            updateMessageActions();
            metrics.finish(QStringLiteral("cache_miss"));
            return;
        }

        const auto cacheMilliseconds = timer.restart();
        applyActiveTabPagePreservingSelection(m_messageSelectionController->currentRow());
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
        if (m_messageListTabController->pageStale(*tab) ||
            (refreshRemote && tabKind(*tab) == TabKind::Mailbox))
        {
            static_cast<void>(m_messageListTabController->refresh(*tab));
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
        if (m_messageListTabController->refresh(tab))
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
        const auto totalThreadsValue =
            currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::TotalThreadsRole);
        const auto totalThreads = totalThreadsValue.isValid()
                                      ? std::optional<std::size_t>{static_cast<std::size_t>(
                                            totalThreadsValue.toULongLong())}
                                      : std::nullopt;
        activateMailboxInHomeTab(
            *accountId, *mailboxId,
            currentIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxNameRole)
                .toString(),
            currentMailboxRole(*m_mailboxView), totalThreads, refreshRemote);
        metrics.finish(QStringLiteral("completed"),
                       QStringLiteral("elapsed_ms=%1").arg(timer.elapsed()));
    }

    void
    MainWindow::markTabsStaleForAccount(const std::string_view accountId,
                                        const std::optional<std::string_view> refreshedMailboxId)
    {
        m_messageListTabController->markTabsStaleForAccount(m_tabs, accountId, refreshedMailboxId);
    }

    void MainWindow::markSearchTabsStaleForAccount(const std::string_view accountId)
    {
        m_messageListTabController->markSearchTabsStaleForAccount(m_tabs, accountId);
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
        const auto result =
            m_messageListTabController->openOrCreateMailbox(m_tabs,
                                                            {
                                                                .accountId = *accountId,
                                                                .mailboxId = *mailboxId,
                                                                .title = title,
                                                                .role = role,
                                                                .sort = m_emailListSort,
                                                                .restored = std::nullopt,
                                                            },
                                                            1);
        m_activeTabIndex = static_cast<int>(result.index);
        updateTabBar();
        activateTab(*m_activeTabIndex, refreshRemote);
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
            messageBox.setWindowTitle(i18n("Close Compose Tab"));
            messageBox.setText(i18n("Save this message as a draft before closing?"));
            QAbstractButton* saveButton =
                messageBox.addButton(i18n("Save Draft"), QMessageBox::AcceptRole);
            QAbstractButton* discardButton = messageBox.addButton(
                i18nc("@action:button", "Discard"), QMessageBox::DestructiveRole);
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

        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(i18n("Select an account before searching."), 5000);
            return;
        }

        openOrActivateSearchTab(*accountId, trimmed, true);
    }

    void MainWindow::showAdvancedSearch()
    {
        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
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
        addPropertyAction(i18n("Date sent"),
                          javelin::jmap::query::EmailListSortProperty::SentAt);
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

        m_emailListSort = std::move(sort);
        m_messageListTabController->setSort(m_tabs, m_emailListSort);
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

    void MainWindow::goToPreviousPage()
    {
        auto* tab = activeTab();
        if (tab == nullptr || !m_messageListTabController->goToPreviousPage(*tab))
            return;

        tabSelection(*tab) = {};
        loadActiveTabFromCache();
    }

    void MainWindow::goToFirstPage()
    {
        goToPage(0);
    }

    void MainWindow::goToLastPage()
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
            return;

        const auto lastPage = m_messageListTabController->lastPageIndex(*tab);
        if (lastPage.has_value())
            goToPage(*lastPage);
    }

    void MainWindow::goToPage(const std::size_t pageIndex)
    {
        auto* tab = activeTab();
        if (tab == nullptr || !m_messageListTabController->goToPage(*tab, pageIndex))
        {
            updateMessageListHeader();
            return;
        }

        tabSelection(*tab) = {};
        loadActiveTabFromCache();
    }

    void MainWindow::goToNextPage()
    {
        auto* tab = activeTab();
        if (tab == nullptr || !m_messageListTabController->goToNextPage(*tab))
            return;

        tabSelection(*tab) = {};
        loadActiveTabFromCache();
    }

    void MainWindow::reloadAccounts()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
    }

    void MainWindow::refreshViewsFromCache()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        loadActiveTabFromCache(true);
    }

    void MainWindow::refreshSelectionFromModels()
    {
        if (activeTabIsCompose())
        {
            updateMessageActions();
            return;
        }

        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        auto selectedSummaries = m_messageSelectionController->selectedMessageSummaries();
        if (selectedSummaries.size() > 1)
        {
            m_messageViewContainer->setMultipleSelection(accountId, mailboxId,
                                                         std::move(selectedSummaries));
            m_messageSelectionController->syncTabSelection(activeTab());
            updateEmptyStates();
            updateMessageListHeader();
            updateMessageActions();
            return;
        }

        const auto emailId = m_messageSelectionController->currentEmailId();
        if (!emailId.has_value())
        {
            if (const auto* route = m_messageNavigationController->activeRoute(activeTab()))
            {
                m_messageViewContainer->setSelection(m_messageViewReader, route->accountId,
                                                     route->mailboxId, route->emailId);
                updateEmptyStates();
                updateMessageListHeader();
                updateMessageActions();
                return;
            }
        }
        m_messageViewContainer->setSelection(m_messageViewReader, accountId, mailboxId, emailId);
        m_messageSelectionController->syncTabSelection(activeTab());
        updateEmptyStates();
        updateMessageListHeader();
        updateMessageActions();
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
        m_messageListTabPresenter->showEmptyState(
            activeTab(), static_cast<std::size_t>(m_messageModel->rowCount()));
    }

    void MainWindow::updateMessageListHeader()
    {
        m_messageListTabPresenter->showHeader(activeTab());
    }

    void MainWindow::updateMessageActions()
    {
        const auto selectedIds = m_messageSelectionController->selectedEmailIds();
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        const auto draftsMailbox =
            accountId.has_value()
                ? findMailboxByRole(m_mailboxReader, *accountId, "drafts")
                : std::optional<javelin::jmap::cache::MailboxTreeItem>{std::nullopt};
        const auto* selectionModel = m_messageView->selectionModel();
        const bool hasReadSelection =
            selectionModel != nullptr &&
            std::ranges::any_of(selectionModel->selectedRows(),
                                [](const QModelIndex& index) { return !indexIsUnread(index); });
        const auto* tab = activeTab();
        const auto actions = messageActionAvailability({
            .tabKind = tab == nullptr ? std::optional<TabKind>{std::nullopt}
                                      : std::optional<TabKind>{tabKind(*tab)},
            .hasAccount = accountId.has_value(),
            .hasMailbox = mailboxId.has_value(),
            .selectedCount = selectedIds.size(),
            .activeMailboxIsDrafts = mailboxId.has_value() && draftsMailbox.has_value() &&
                                     *mailboxId == draftsMailbox->id,
            .hasReadSelection = hasReadSelection,
        });

        m_newMessageAction->setEnabled(actions.newMessage);
        m_replyAction->setEnabled(actions.reply);
        m_replyAllAction->setEnabled(actions.replyAll);
        m_forwardAction->setEnabled(actions.forward);
        m_editDraftAction->setEnabled(actions.editDraft);
        m_archiveAction->setEnabled(actions.archive);
        m_markUnreadAction->setEnabled(actions.markUnread);
        m_deleteAction->setEnabled(actions.deleteFromMailbox);
        m_permanentDeleteAction->setEnabled(actions.permanentDelete);
        m_moveAction->setEnabled(actions.move);
        m_copyAction->setEnabled(actions.copy);
        m_viewSourceAction->setEnabled(actions.viewSource);
    }

    void MainWindow::updateSortButton()
    {
        if (m_messageSortButton == nullptr)
        {
            return;
        }

        m_messageSortButton->setToolTip(
            i18n("Sort messages: %1, %2", sortPropertyLabel(m_emailListSort.property),
                 sortDirectionLabel(m_emailListSort.direction)));
    }

    void MainWindow::scheduleApplicationPaletteRefresh()
    {
        if (m_paletteRefreshPending)
        {
            return;
        }

        m_paletteRefreshPending = true;
        QTimer::singleShot(0, this,
                           [this]
                           {
                               m_paletteRefreshPending = false;
                               applyApplicationPalette();
                           });
    }

    void MainWindow::applyApplicationPalette()
    {
        // Widgets created while the window has a resolved application palette can retain those
        // roles as explicit state, especially through QStyleSheetStyle. Clear every resolved
        // widget palette so the hierarchy inherits the new application palette again.
        const auto descendants = findChildren<QWidget*>();
        for (auto widget = descendants.crbegin(); widget != descendants.crend(); ++widget)
        {
            (*widget)->setPalette(QPalette{});
        }
        setPalette(QPalette{});

        updatePaletteDependentIcons();
        updateTabBar();
        m_calendarTabController->applicationPaletteChanged();
        m_contactsTabController->applicationPaletteChanged();
        m_mailboxView->viewport()->update();
        m_messageView->viewport()->update();
        update();
    }

    void MainWindow::updatePaletteDependentIcons()
    {
        const auto iconColor = palette().color(QPalette::Active, QPalette::Text);
        const auto icon = [iconColor](const QString& resourcePath)
        { return javelin::gui::themedSvgIcon(resourcePath, iconColor); };

        m_refreshAction->setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/cloud-download.svg")));
        m_preferencesAction->setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/settings.svg")));
        m_newMessageAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/new-mail.svg")));
        m_replyAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/reply.svg")));
        m_replyAllAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/reply-all.svg")));
        m_forwardAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/forward.svg")));
        m_editDraftAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/draft.svg")));
        m_archiveAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/archive.svg")));
        m_markUnreadAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/unread.svg")));
        m_deleteAction->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/delete.svg")));
        m_advancedSearchAction->setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/search.svg")));

        m_messageSortButton->setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/display-options.svg")));
        m_previousPageButton->setIcon(
            icon(QStringLiteral(":/icons/thunderbird-icons/nav-left.svg")));
        m_nextPageButton->setIcon(icon(QStringLiteral(":/icons/thunderbird-icons/nav-right.svg")));
    }

    void MainWindow::changeEvent(QEvent* event)
    {
        KXmlGuiWindow::changeEvent(event);
        if (event->type() == QEvent::ApplicationPaletteChange)
        {
            scheduleApplicationPaletteRefresh();
        }
    }

    bool MainWindow::eventFilter(QObject* watched, QEvent* event)
    {
        if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange)
        {
            scheduleApplicationPaletteRefresh();
        }

        if (event->type() == QEvent::KeyRelease || event->type() == QEvent::InputMethod ||
            event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
            event->type() == QEvent::MouseButtonRelease)
        {
            QTimer::singleShot(0, this, &MainWindow::updateUndoRedoActions);
        }

        if (watched == m_messageView->viewport() && event->type() == QEvent::MouseButtonPress)
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

    void MainWindow::openPreferences()
    {
        openPreferencesForConnection({});
    }

    void MainWindow::reauthenticateConnection(const QString& connectionId)
    {
        javelin::gui::onboarding::FirstRunWizard wizard{m_onboardingPort, m_settings, connectionId,
                                                        this};
        if (wizard.exec() != QDialog::Accepted)
            return;

        const auto accounts = m_settings.accounts();
        const auto account = std::ranges::find(accounts, connectionId,
                                               &javelin::gui::settings::ConnectionSettings::id);
        if (account != accounts.end())
            m_accountRefreshController->refreshConnection(*account);
    }

    void MainWindow::updateAuthenticationPrompt(const QString& accountId,
                                                const javelin::app::MailAccountStatus status)
    {
        const auto account = m_settings.accountForCachedId(accountId);
        if (account.id.isEmpty())
            return;

        if (status == javelin::app::MailAccountStatus::AuthenticationPaused)
            m_authenticationRequiredAccountIds.insert(accountId);
        else
            m_authenticationRequiredAccountIds.remove(accountId);

        const bool connectionRequiresAuthentication = std::ranges::any_of(
            account.cachedAccountIds, [this](const QString& cachedAccountId)
            { return m_authenticationRequiredAccountIds.contains(cachedAccountId); });
        if (!connectionRequiresAuthentication)
        {
            m_authenticationPromptedConnections.remove(account.id);
            m_pendingAuthenticationPrompts.removeAll(account.id);
            return;
        }

        if (m_authenticationPromptedConnections.contains(account.id))
            return;
        m_authenticationPromptedConnections.insert(account.id);
        m_pendingAuthenticationPrompts.push_back(account.id);
        QTimer::singleShot(0, this, &MainWindow::showNextAuthenticationPrompt);
    }

    void MainWindow::showNextAuthenticationPrompt()
    {
        if (m_authenticationPromptOpen)
            return;
        while (!m_pendingAuthenticationPrompts.isEmpty())
        {
            const auto connectionId = m_pendingAuthenticationPrompts.takeFirst();
            const auto accounts = m_settings.accounts();
            const auto account = std::ranges::find(accounts, connectionId,
                                                   &javelin::gui::settings::ConnectionSettings::id);
            if (account == accounts.end())
                continue;
            const bool connectionRequiresAuthentication = std::ranges::any_of(
                account->cachedAccountIds, [this](const QString& accountId)
                { return m_authenticationRequiredAccountIds.contains(accountId); });
            if (!connectionRequiresAuthentication)
                continue;

            const auto accountName =
                account->displayName.isEmpty() ? account->loginEmail : account->displayName;
            QMessageBox prompt{
                QMessageBox::Warning, i18n("Sign in required"),
                i18n("%1 needs you to sign in again before Javelin can synchronize mail.",
                     accountName),
                QMessageBox::NoButton, this};
            auto* signInButton =
                prompt.addButton(i18n("Sign In Again"), QMessageBox::AcceptRole);
            prompt.addButton(i18nc("@action:button", "Later"), QMessageBox::RejectRole);
            m_authenticationPromptOpen = true;
            prompt.exec();
            m_authenticationPromptOpen = false;
            if (prompt.clickedButton() == signInButton)
                reauthenticateConnection(connectionId);
            break;
        }

        if (!m_pendingAuthenticationPrompts.isEmpty())
            QTimer::singleShot(0, this, &MainWindow::showNextAuthenticationPrompt);
    }

    void MainWindow::openPreferencesForConnection(const QString& connectionId)
    {
        javelin::gui::settings::PreferencesDialog dialog{m_settings,
                                                         m_accountCommandPort,
                                                         m_onboardingPort,
                                                         m_translationService,
                                                         m_accountReader,
                                                         m_mailboxReader,
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
            const auto accountId = activeAccountId().has_value() ? activeAccountId()
                                                                 : currentAccountId(*m_mailboxView);
            if (accountId.has_value())
            {
                refreshAccountFromServer(*accountId);
            }
        }
    }

    void MainWindow::refreshFromServer()
    {
        if (activeTabIsSearch())
        {
            refreshActiveTabFromServer();
            return;
        }

        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
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

    void MainWindow::showMessageListContextMenu(const QPoint& position)
    {
        const QModelIndex index = m_messageView->indexAt(position);
        if (!index.isValid())
        {
            return;
        }

        const auto accountId = activeAccountId();
        const auto sourceMailboxId = activeMailboxId();
        const auto clickedEmailId =
            index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                .toString()
                .toStdString();
        if (!accountId.has_value() || clickedEmailId.empty())
        {
            return;
        }

        if (!m_messageView->selectionModel()->isSelected(index))
        {
            m_messageView->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect |
                                                               QItemSelectionModel::Rows);
            m_messageView->setCurrentIndex(index);
        }
        const auto selection = m_messageCommandController->selectedActionItems();

        QMenu menu{this};
        menu.addAction(m_viewSourceAction);
        menu.addAction(m_markUnreadAction);
        const auto senderEmail =
            index.data(javelin::gui::messages::MessageListModel::SenderEmailRole)
                .toString()
                .trimmed();
        if (!senderEmail.isEmpty())
        {
            auto* findSenderAction =
                menu.addAction(i18n("Find all conversations with %1", senderEmail));
            connect(findSenderAction, &QAction::triggered, this,
                    [this, index] { findConversationsWithSender(index); });
        }
        if (sourceMailboxId.has_value() || activeTabIsSearch())
        {
            menu.addSeparator();
            menu.addAction(m_archiveAction);
            if (sourceMailboxId.has_value())
            {
                menu.addAction(m_deleteAction);
                menu.addAction(m_permanentDeleteAction);
            }
            menu.addSeparator();
            auto* moveMenu = menu.addMenu(i18n("Move to"));
            auto* copyMenu = menu.addMenu(i18n("Copy to"));
            m_messageCommandController->populateDestinationMenus(moveMenu, copyMenu, *accountId,
                                                                 sourceMailboxId, selection);
            if (moveMenu->actions().empty())
            {
                moveMenu->setEnabled(false);
            }
            if (copyMenu->actions().empty())
            {
                copyMenu->setEnabled(false);
            }
        }

        menu.exec(m_messageView->viewport()->mapToGlobal(position));
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

        m_mailboxView->setCurrentIndex(index);

        QMenu menu{this};
        if (mailboxId.isEmpty())
        {
            auto* refreshAccountAction = menu.addAction(i18n("Refresh Account"));
            connect(refreshAccountAction, &QAction::triggered, this,
                    [this, account = accountId.toStdString()]
                    { refreshAccountFromServer(account); });
            menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
            return;
        }
        auto* openAsTabAction = menu.addAction(i18n("Open as Tab"));
        connect(openAsTabAction, &QAction::triggered, this,
                [this] { openMailboxSelectionInTab(true); });
        menu.addSeparator();
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

                javelin::gui::mailboxes::MailboxPropertiesDialog dialog{accountId, *mailbox, this};
                dialog.exec();
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

        m_emailListSort = state.emailListSort;
        updateSortButton();

        m_tabs.clear();
        m_tabs.reserve(state.tabs.size());
        for (auto& tab : state.tabs)
        {
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

        m_activeTabIndex = std::clamp(state.activeTabIndex, 0, static_cast<int>(m_tabs.size() - 1));
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
        refreshTabFromServer(static_cast<std::size_t>(*m_activeTabIndex));
    }

    void MainWindow::restoreMailboxTab(const PersistedMailboxTab& tab)
    {
        auto plan = planMailboxTabRestore(tab, pageSize);
        auto restoredTab = m_messageListTabController->createMailboxTab({
            .accountId = std::move(plan.accountId),
            .mailboxId = std::move(plan.mailboxId),
            .title = std::move(plan.title),
            .role = std::move(plan.mailboxRole),
            .sort = m_emailListSort,
            .restored = std::move(plan.restored),
        });
        tabSelection(restoredTab) = std::move(plan.selection);
        m_tabs.push_back(std::move(restoredTab));
    }

    void MainWindow::restoreSearchTab(PersistedSearchTab tab)
    {
        auto plan = planSearchTabRestore(std::move(tab));
        auto restoredTab = m_messageListTabController->createSearchTab({
            .accountId = std::move(plan.accountId),
            .criteria = std::move(plan.criteria),
            .sort = m_emailListSort,
            .restored = std::move(plan.restored),
        });
        tabSelection(restoredTab) = std::move(plan.selection);
        m_tabs.push_back(std::move(restoredTab));
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
