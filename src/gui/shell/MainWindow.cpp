#include "gui/shell/MainWindow.h"

#include "app/ComposeService.h"
#include "app/MailApplicationService.h"
#include "app/MailboxSession.h"
#include "app/MessageListSession.h"
#include "app/MessageNavigationCoordinator.h"
#include "app/SearchSession.h"
#include "app/TranslationService.h"
#include "gui/IconUtils.h"
#include "gui/calendar/CalendarPresentation.h"
#include "gui/calendar/EventDialog.h"
#include "gui/calendar/MonthCalendarWidget.h"
#include "gui/compose/ComposeTabWidget.h"
#include "gui/contacts/ContactsManagerWidget.h"
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
#include "gui/messages/MessageSelectionRestoration.h"
#include "gui/messageview/MessageViewContainer.h"
#include "gui/search/AdvancedSearchDialog.h"
#include "gui/settings/PreferencesDialog.h"
#include "gui/shell/ElidingLabel.h"
#include "gui/shell/LayeredStatusBar.h"
#include "gui/shell/MainWindowStateStore.h"
#include "gui/shell/MessageActionPolicy.h"
#include "gui/shell/MessageCommandController.h"
#include "gui/shell/MessageFileController.h"
#include "gui/shell/MessageListTabBindingPresenter.h"
#include "gui/shell/MessageListTabController.h"
#include "gui/shell/MessageListTabPresenter.h"
#include "gui/shell/MessageNavigationController.h"
#include "gui/shell/TabActivationPolicy.h"
#include "gui/shell/TabBarPresenter.h"
#include "gui/shell/TabPersistence.h"
#include "gui/sieve/SieveEditorDialog.h"
#include "jmap/cache/AccountRepository.h"
#include "jmap/cache/ContactRepository.h"
#include "jmap/cache/IdentityRepository.h"
#include "jmap/cache/MessageViewService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/calendar/CalendarEventEditing.h"
#include "jmap/calendar/CalendarService.h"
#include "jmap/contacts/ContactIdentityLookup.h"
#include "jmap/contacts/ContactService.h"
#include "jmap/sync/MailboxQueryDescriptor.h"

#include <QCoroTask>

#include <KActionCollection>
#include <KStandardAction>

#include <QAbstractButton>
#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
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
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

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
        QMessageBox::critical(this, QStringLiteral("Action Required"), message);
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

        [[nodiscard]] std::optional<std::string> currentEmailId(const QListView& messageView)
        {
            const auto currentIndex = messageView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto emailId =
                currentIndex.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
            return emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                     : std::optional<std::string>{emailId.toStdString()};
        }

        [[nodiscard]] std::optional<std::string> currentThreadId(const QListView& messageView)
        {
            const auto currentIndex = messageView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            const auto threadId =
                currentIndex.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                    .toString();
            return threadId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                      : std::optional<std::string>{threadId.toStdString()};
        }

        [[nodiscard]] std::optional<int> currentMessageRow(const QListView& messageView)
        {
            const auto currentIndex = messageView.currentIndex();
            if (!currentIndex.isValid())
            {
                return std::nullopt;
            }

            return currentIndex.row();
        }

        [[nodiscard]] std::vector<javelin::gui::messages::MessageRowIdentity>
        messageRowIdentities(const javelin::gui::messages::MessageListModel& model)
        {
            std::vector<javelin::gui::messages::MessageRowIdentity> rows;
            rows.reserve(static_cast<std::size_t>(model.rowCount()));
            for (int row = 0; row < model.rowCount(); ++row)
            {
                const auto index = model.index(row, 0);
                rows.push_back({
                    .threadId = index.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                                    .toString()
                                    .toStdString(),
                    .emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                                   .toString()
                                   .toStdString(),
                });
            }
            return rows;
        }

        [[nodiscard]] bool indexIsUnread(const QModelIndex& index)
        {
            return index.isValid() &&
                   index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool();
        }

        [[nodiscard]] QModelIndex findIndexByRole(const QAbstractItemModel& model, const int role,
                                                  const QString& value,
                                                  const QModelIndex& parent = {})
        {
            const int rowCount = model.rowCount(parent);
            for (int row = 0; row < rowCount; ++row)
            {
                const QModelIndex index = model.index(row, 0, parent);
                if (!index.isValid())
                {
                    continue;
                }

                if (index.data(role).toString() == value)
                {
                    return index;
                }

                const QModelIndex childMatch = findIndexByRole(model, role, value, index);
                if (childMatch.isValid())
                {
                    return childMatch;
                }
            }

            return {};
        }

        using javelin::gui::mailboxes::findMailboxIndexForSelection;

        [[nodiscard]] javelin::app::AccountConnectionSettings
        toAccountConnectionSettings(const javelin::gui::settings::ConnectionSettings& settings)
        {
            return javelin::app::AccountConnectionSettings{
                .connectionId = settings.id.toStdString(),
                .revision = settings.revision,
                .sessionUrl = settings.sessionUrl.toStdString(),
                .loginEmail = settings.loginEmail.toStdString(),
                .apiKey = settings.apiKey.toStdString(),
            };
        }

        [[nodiscard]] std::optional<javelin::jmap::cache::MailboxTreeItem>
        findMailboxByRole(javelin::jmap::cache::QueryService& queryService,
                          const std::string_view accountId, const std::string_view role)
        {
            const auto result = queryService.listMailboxTree(accountId);
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
                return QStringLiteral("Date received");
            case javelin::jmap::query::EmailListSortProperty::SentAt:
                return QStringLiteral("Date sent");
            case javelin::jmap::query::EmailListSortProperty::From:
                return QStringLiteral("From");
            case javelin::jmap::query::EmailListSortProperty::To:
                return QStringLiteral("To");
            case javelin::jmap::query::EmailListSortProperty::Subject:
                return QStringLiteral("Subject");
            case javelin::jmap::query::EmailListSortProperty::Size:
                return QStringLiteral("Size");
            }

            return QStringLiteral("Date received");
        }

        [[nodiscard]] QString
        sortDirectionLabel(const javelin::jmap::query::EmailListSortDirection direction)
        {
            return direction == javelin::jmap::query::EmailListSortDirection::Ascending
                       ? QStringLiteral("ascending")
                       : QStringLiteral("descending");
        }

    } // namespace

    MainWindow::MainWindow(javelin::jmap::cache::AccountRepository& accountRepository,
                           javelin::jmap::cache::ContactRepository& contactRepository,
                           javelin::jmap::contacts::ContactService& contactService,
                           javelin::jmap::calendar::CalendarService& calendarService,
                           javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
                           javelin::jmap::cache::IdentityRepository& identityRepository,
                           javelin::jmap::cache::MessageViewService& messageViewService,
                           javelin::jmap::cache::QueryService& queryService,
                           javelin::app::TranslationService& translationService,
                           javelin::app::ComposeService& composeService,
                           javelin::app::MailApplicationService& mailService,
                           javelin::app::MessageNavigationCoordinator& messageNavigationCoordinator,
                           QWidget* parent)
        : KXmlGuiWindow(parent), m_accountRepository(accountRepository),
          m_contactRepository(contactRepository), m_contactService(contactService),
          m_calendarService(calendarService), m_contactIdentityLookup(contactIdentityLookup),
          m_identityRepository(identityRepository), m_messageViewService(messageViewService),
          m_queryService(queryService), m_translationService(translationService),
          m_composeService(composeService), m_mailService(mailService),
          m_messageNavigationCoordinator(messageNavigationCoordinator)
    {
        m_statusBar = new LayeredStatusBar(this);
        setStatusBar(m_statusBar);
        m_messageFileController =
            new MessageFileController(m_mailService, m_messageViewService, this, this);
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
        m_messageListTabBindingPresenter = std::make_unique<MessageListTabBindingPresenter>(
            *m_mailboxModel, *m_mailboxView, *m_mailboxSearchEdit, *m_messageModel, *m_mailboxPane);
        m_messageListTabController =
            new MessageListTabController(m_queryService, m_mailService, pageSize, this, this);
        m_messageNavigationController = std::make_unique<MessageNavigationController>(
            m_messageNavigationCoordinator, *m_messageListTabController);
        connect(m_messageListTabController, &MessageListTabController::pageChanged, this,
                [this](javelin::app::MessageListSession* session)
                {
                    const auto* tab = activeTab();
                    if (tab == nullptr || !m_messageListTabController->ownsSession(*tab, session))
                        return;

                    applyActiveTabPagePreservingSelection(currentMessageRow(*m_messageView));
                    updateEmptyStates();
                    updateMessageListHeader();
                    resolveOpenEmailRoute();
                });
        connect(m_messageListTabController, &MessageListTabController::operationFailed, this,
                [this](const javelin::jmap::OperationError& error) { presentError(error); });
        connect(&m_mailService, &javelin::app::MailApplicationService::sessionCapabilitiesChanged,
                this, [this](const QString&) { reloadAccounts(); });
        createActions();
        setupGUI(KXmlGuiWindow::ToolBar | KXmlGuiWindow::Keys | KXmlGuiWindow::Save |
                     KXmlGuiWindow::Create,
                 QStringLiteral("javelinmailui.rc"));
        updateToolbarForActiveTab();
        connectSelection();
        connect(&m_messageNavigationCoordinator,
                &javelin::app::MessageNavigationCoordinator::routeRequested, this,
                &MainWindow::openEmailRoute);
        connect(&m_mailService, &javelin::app::MailApplicationService::accountStatusChanged, this,
                [this](const QString& accountId, const auto status)
                {
                    using Model = javelin::gui::mailboxes::MailboxTreeModel;
                    Model::ConnectionStatus modelStatus = Model::ConnectionStatus::Disconnected;
                    if (status == javelin::app::AccountSyncCoordinator::Status::Connecting)
                    {
                        modelStatus = Model::ConnectionStatus::Connecting;
                    }
                    else if (status == javelin::app::AccountSyncCoordinator::Status::Connected)
                    {
                        modelStatus = Model::ConnectionStatus::Connected;
                    }
                    else if (status ==
                             javelin::app::AccountSyncCoordinator::Status::AuthenticationPaused)
                    {
                        modelStatus = Model::ConnectionStatus::AuthenticationPaused;
                    }
                    m_mailboxModel->setConnectionStatus(accountId, modelStatus);
                });
        connect(&m_mailService, &javelin::app::MailApplicationService::cacheCommitted, this,
                [this](const javelin::app::MailCacheChange& change)
                {
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
        std::optional<std::string> mailboxRole;
        std::optional<std::size_t> totalThreads;
        const auto mailboxIndex = findMailboxIndexForSelection(*m_mailboxModel, accountId,
                                                               std::optional<QString>{mailboxId});
        if (mailboxIndex.isValid())
        {
            mailboxTitle =
                mailboxIndex.data(javelin::gui::mailboxes::MailboxTreeModel::MailboxNameRole)
                    .toString();
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
            activeTab(), messageRowIdentities(*m_messageModel));
        if (!resolution.route.has_value())
            return;

        const auto& route = *resolution.route;
        if (resolution.currentRow.has_value())
        {
            const auto index = m_messageModel->index(static_cast<int>(*resolution.currentRow), 0);
            if (index.isValid())
            {
                m_messageView->setCurrentIndex(index);
                m_messageView->scrollTo(index);
                syncActiveTabSelectionFromViews();
            }
        }

        m_messageViewContainer->setSelection(m_messageViewService, route.accountId, route.mailboxId,
                                             route.emailId);
        if (!m_messageViewContainer->hasReadableBody())
        {
            m_messageViewContainer->setLoadingState(true);
            refreshSelectedMessageContent(route.accountId, route.emailId);
        }
    }

    void MainWindow::createActions()
    {
        const auto iconColor = palette().color(QPalette::Text);
        auto thunderbirdIcon = [iconColor](const QString& resourcePath)
        { return javelin::gui::themedSvgIcon(resourcePath, iconColor); };

        m_refreshAction = new QAction(
            thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/cloud-download.svg")),
            QStringLiteral("Refresh From Server"), this);
        m_refreshAction->setShortcut(QKeySequence::Refresh);
        connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshFromServer);
        actionCollection()->addAction(QStringLiteral("refresh_from_server"), m_refreshAction);
        actionCollection()->setDefaultShortcut(m_refreshAction, QKeySequence::Refresh);

        m_quitAction = new QAction(QIcon::fromTheme(QStringLiteral("application-exit")),
                                   QStringLiteral("&Quit"), this);
        m_quitAction->setShortcut(QKeySequence::Quit);
        connect(m_quitAction, &QAction::triggered, qApp, &QCoreApplication::quit);
        actionCollection()->addAction(QStringLiteral("quit_application"), m_quitAction);
        actionCollection()->setDefaultShortcut(m_quitAction, QKeySequence::Quit);

        m_preferencesAction =
            KStandardAction::preferences(this, &MainWindow::openPreferences, actionCollection());
        m_preferencesAction->setIcon(
            thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/settings.svg")));

        m_contactsAction = new QAction(QIcon::fromTheme(QStringLiteral("view-pim-contacts")),
                                       QStringLiteral("Contacts"), this);
        connect(m_contactsAction, &QAction::triggered, this, &MainWindow::openContacts);
        actionCollection()->addAction(QStringLiteral("open_contacts"), m_contactsAction);
        const auto contactAccounts = m_contactRepository.listAccounts();
        m_contactsAction->setEnabled(
            std::holds_alternative<std::vector<javelin::jmap::cache::ContactAccount>>(
                contactAccounts) &&
            !std::get<std::vector<javelin::jmap::cache::ContactAccount>>(contactAccounts).empty());

        m_calendarAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-month")),
                                       QStringLiteral("Calendar"), this);
        connect(m_calendarAction, &QAction::triggered, this, &MainWindow::openCalendar);
        actionCollection()->addAction(QStringLiteral("open_calendar"), m_calendarAction);
        const auto calendarAccounts = m_calendarService.accounts();
        m_calendarAction->setEnabled(
            std::holds_alternative<std::vector<javelin::jmap::cache::CalendarAccount>>(
                calendarAccounts) &&
            !std::get<std::vector<javelin::jmap::cache::CalendarAccount>>(calendarAccounts)
                 .empty());

        m_sieveAction = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                    QStringLiteral("Sieve Rules"), this);
        connect(m_sieveAction, &QAction::triggered, this, &MainWindow::openSieveEditor);
        actionCollection()->addAction(QStringLiteral("open_sieve_editor"), m_sieveAction);

        m_newMessageAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/new-mail.svg")),
                        QStringLiteral("&New Message"), this);
        m_newMessageAction->setShortcut(QKeySequence::New);
        connect(m_newMessageAction, &QAction::triggered, this, &MainWindow::composeNewMessage);
        actionCollection()->addAction(QStringLiteral("compose_new_message"), m_newMessageAction);
        actionCollection()->setDefaultShortcut(m_newMessageAction, QKeySequence::New);

        m_replyAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/reply.svg")),
                        QStringLiteral("&Reply"), this);
        m_replyAction->setShortcut(QKeySequence{Qt::Key_R});
        connect(m_replyAction, &QAction::triggered, this, &MainWindow::composeReply);
        actionCollection()->addAction(QStringLiteral("compose_reply"), m_replyAction);
        actionCollection()->setDefaultShortcut(m_replyAction, QKeySequence{Qt::Key_R});

        m_replyAllAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/reply-all.svg")),
                        QStringLiteral("Reply &All"), this);
        connect(m_replyAllAction, &QAction::triggered, this, &MainWindow::composeReplyAll);
        actionCollection()->addAction(QStringLiteral("compose_reply_all"), m_replyAllAction);

        m_forwardAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/forward.svg")),
                        QStringLiteral("&Forward"), this);
        connect(m_forwardAction, &QAction::triggered, this, &MainWindow::composeForward);
        actionCollection()->addAction(QStringLiteral("compose_forward"), m_forwardAction);

        m_editDraftAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/draft.svg")),
                        QStringLiteral("Edit &Draft"), this);
        connect(m_editDraftAction, &QAction::triggered, this, &MainWindow::editSelectedDraft);
        actionCollection()->addAction(QStringLiteral("compose_edit_draft"), m_editDraftAction);

        m_archiveAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/archive.svg")),
                        QStringLiteral("&Archive"), this);
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
                        QStringLiteral("Mark &Unread"), this);
        connect(m_markUnreadAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->markSelectionUnread(activeAccountId(),
                                                                    activeMailboxId());
                });
        actionCollection()->addAction(QStringLiteral("mark_email_unread"), m_markUnreadAction);

        m_deleteAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/delete.svg")),
                        QStringLiteral("&Delete"), this);
        m_deleteAction->setShortcut(QKeySequence::Delete);
        connect(
            m_deleteAction, &QAction::triggered, this, [this]
            { m_messageCommandController->deleteSelection(activeAccountId(), activeMailboxId()); });
        actionCollection()->addAction(QStringLiteral("delete_email"), m_deleteAction);
        actionCollection()->setDefaultShortcut(m_deleteAction, QKeySequence::Delete);

        m_permanentDeleteAction = new QAction(QStringLiteral("Delete Permanently"), this);
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
                                   QStringLiteral("&Move to…"), this);
        connect(m_moveAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->showTransferMenu(
                        MessageTransferOperation::Move, activeAccountId(), activeMailboxId(),
                        activeTabIsSearch());
                });
        actionCollection()->addAction(QStringLiteral("move_email"), m_moveAction);

        m_copyAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                   QStringLiteral("&Copy to…"), this);
        connect(m_copyAction, &QAction::triggered, this,
                [this]
                {
                    m_messageCommandController->showTransferMenu(
                        MessageTransferOperation::Copy, activeAccountId(), activeMailboxId(),
                        activeTabIsSearch());
                });
        actionCollection()->addAction(QStringLiteral("copy_email"), m_copyAction);

        m_viewSourceAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                         QStringLiteral("View &Source"), this);
        connect(m_viewSourceAction, &QAction::triggered, this,
                &MainWindow::viewSelectedMessageSource);
        actionCollection()->addAction(QStringLiteral("view_message_source"), m_viewSourceAction);

        m_advancedSearchAction =
            new QAction(thunderbirdIcon(QStringLiteral(":/icons/thunderbird-icons/search.svg")),
                        QStringLiteral("Advanced Search"), this);
        connect(m_advancedSearchAction, &QAction::triggered, this, &MainWindow::showAdvancedSearch);
        actionCollection()->addAction(QStringLiteral("advanced_search"), m_advancedSearchAction);

        m_composeSendAction = new QAction(QIcon::fromTheme(QStringLiteral("mail-send")),
                                          QStringLiteral("Send"), this);
        connect(m_composeSendAction, &QAction::triggered, this,
                [this]
                {
                    if (auto* tab = activeTab())
                        if (auto* compose = std::get_if<ComposeTabState>(&tab->content);
                            compose != nullptr && compose->widget != nullptr)
                            compose->widget->sendMessage();
                });
        actionCollection()->addAction(QStringLiteral("compose_send"), m_composeSendAction);

        m_composeSaveDraftAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                               QStringLiteral("Save Draft"), this);
        connect(m_composeSaveDraftAction, &QAction::triggered, this,
                [this]
                {
                    if (auto* tab = activeTab())
                        if (auto* compose = std::get_if<ComposeTabState>(&tab->content);
                            compose != nullptr && compose->widget != nullptr)
                            compose->widget->saveDraft();
                });
        actionCollection()->addAction(QStringLiteral("compose_save_draft"),
                                      m_composeSaveDraftAction);

        m_composeAttachFilesAction =
            new QAction(QIcon::fromTheme(QStringLiteral("mail-attachment")),
                        QStringLiteral("Attach Files"), this);
        connect(m_composeAttachFilesAction, &QAction::triggered, this,
                [this]
                {
                    if (auto* tab = activeTab())
                        if (auto* compose = std::get_if<ComposeTabState>(&tab->content);
                            compose != nullptr && compose->widget != nullptr)
                            compose->widget->attachFiles();
                });
        actionCollection()->addAction(QStringLiteral("compose_attach_files"),
                                      m_composeAttachFilesAction);

        const auto invokeContact = [this](const auto operation)
        {
            if (auto* tab = activeTab())
                if (auto* contacts = std::get_if<ContactsTabState>(&tab->content);
                    contacts != nullptr && contacts->widget != nullptr)
                    (contacts->widget->*operation)();
        };
        m_contactNewAction = new QAction(QIcon::fromTheme(QStringLiteral("contact-new")),
                                         QStringLiteral("Add"), this);
        connect(
            m_contactNewAction, &QAction::triggered, this, [invokeContact]
            { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::beginCreateContact); });
        auto* contactAddMenu = new QMenu(this);
        auto* newContact = contactAddMenu->addAction(
            QIcon::fromTheme(QStringLiteral("contact-new")), QStringLiteral("New Contact"));
        connect(
            newContact, &QAction::triggered, this, [invokeContact]
            { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::beginCreateContact); });
        auto* newGroup = contactAddMenu->addAction(QIcon::fromTheme(QStringLiteral("system-users")),
                                                   QStringLiteral("New Group"));
        connect(
            newGroup, &QAction::triggered, this, [invokeContact]
            { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::beginCreateGroup); });
        m_contactNewAction->setMenu(contactAddMenu);
        actionCollection()->addAction(QStringLiteral("contact_new"), m_contactNewAction);
        m_contactEditAction = new QAction(QIcon::fromTheme(QStringLiteral("document-edit")),
                                          QStringLiteral("Edit Contact"), this);
        connect(
            m_contactEditAction, &QAction::triggered, this, [invokeContact]
            { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::beginEditContact); });
        actionCollection()->addAction(QStringLiteral("contact_edit"), m_contactEditAction);
        m_contactDeleteAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-delete")),
                                            QStringLiteral("Delete Contact"), this);
        connect(m_contactDeleteAction, &QAction::triggered, this, [invokeContact]
                { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::deleteContact); });
        actionCollection()->addAction(QStringLiteral("contact_delete"), m_contactDeleteAction);
        m_contactCopyAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                          QStringLiteral("Copy Contact"), this);
        connect(m_contactCopyAction, &QAction::triggered, this, [invokeContact]
                { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::copyContact); });
        actionCollection()->addAction(QStringLiteral("contact_copy"), m_contactCopyAction);
        m_contactImportAction = new QAction(QIcon::fromTheme(QStringLiteral("document-import")),
                                            QStringLiteral("Import vCard…"), this);
        connect(m_contactImportAction, &QAction::triggered, this, [invokeContact]
                { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::importVCard); });
        actionCollection()->addAction(QStringLiteral("contact_import"), m_contactImportAction);
        m_contactExportAction = new QAction(QIcon::fromTheme(QStringLiteral("document-export")),
                                            QStringLiteral("Export vCard…"), this);
        connect(m_contactExportAction, &QAction::triggered, this, [invokeContact]
                { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::exportVCard); });
        actionCollection()->addAction(QStringLiteral("contact_export"), m_contactExportAction);
        m_contactDuplicatesAction = new QAction(QIcon::fromTheme(QStringLiteral("merge")),
                                                QStringLiteral("Find Duplicates…"), this);
        connect(m_contactDuplicatesAction, &QAction::triggered, this,
                [invokeContact]
                {
                    invokeContact(
                        &javelin::gui::contacts::ContactsManagerWidget::findAndMergeDuplicates);
                });
        actionCollection()->addAction(QStringLiteral("contact_duplicates"),
                                      m_contactDuplicatesAction);
        m_contactAddToGroupAction = new QAction(QIcon::fromTheme(QStringLiteral("list-add")),
                                                QStringLiteral("Add to Group"), this);
        auto* addToGroupMenu = new QMenu(this);
        connect(addToGroupMenu, &QMenu::aboutToShow, this,
                [this, addToGroupMenu]
                {
                    const auto* tab = activeTab();
                    const auto* contacts =
                        tab == nullptr ? nullptr : std::get_if<ContactsTabState>(&tab->content);
                    if (contacts != nullptr && contacts->widget != nullptr)
                        contacts->widget->populateAddToGroupMenu(*addToGroupMenu);
                    else
                        addToGroupMenu->clear();
                });
        m_contactAddToGroupAction->setMenu(addToGroupMenu);
        actionCollection()->addAction(QStringLiteral("contact_add_to_group"),
                                      m_contactAddToGroupAction);
        m_contactRemoveFromGroupAction =
            new QAction(QIcon::fromTheme(QStringLiteral("list-remove")),
                        QStringLiteral("Remove from Group"), this);
        auto* removeFromGroupMenu = new QMenu(this);
        connect(removeFromGroupMenu, &QMenu::aboutToShow, this,
                [this, removeFromGroupMenu]
                {
                    const auto* tab = activeTab();
                    const auto* contacts =
                        tab == nullptr ? nullptr : std::get_if<ContactsTabState>(&tab->content);
                    if (contacts != nullptr && contacts->widget != nullptr)
                        contacts->widget->populateRemoveFromGroupMenu(*removeFromGroupMenu);
                    else
                        removeFromGroupMenu->clear();
                });
        m_contactRemoveFromGroupAction->setMenu(removeFromGroupMenu);
        actionCollection()->addAction(QStringLiteral("contact_remove_from_group"),
                                      m_contactRemoveFromGroupAction);
        m_contactManageAddressBooksAction =
            new QAction(QIcon::fromTheme(QStringLiteral("view-list-details")),
                        QStringLiteral("Manage Address Books…"), this);
        connect(m_contactManageAddressBooksAction, &QAction::triggered, this,
                [invokeContact]
                {
                    invokeContact(
                        &javelin::gui::contacts::ContactsManagerWidget::showAddressBookManager);
                });
        actionCollection()->addAction(QStringLiteral("contact_manage_address_books"),
                                      m_contactManageAddressBooksAction);
        m_contactRefreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                             QStringLiteral("Refresh Contacts"), this);
        connect(m_contactRefreshAction, &QAction::triggered, this, [invokeContact]
                { invokeContact(&javelin::gui::contacts::ContactsManagerWidget::requestRefresh); });
        actionCollection()->addAction(QStringLiteral("contact_refresh"), m_contactRefreshAction);

        const auto invokeCalendar = [this](const auto operation)
        {
            if (auto* tab = activeTab())
                if (auto* calendar = std::get_if<CalendarTabState>(&tab->content);
                    calendar != nullptr && calendar->widget != nullptr)
                    (calendar->widget->*operation)();
        };
        m_calendarNewEventAction = new QAction(QIcon::fromTheme(QStringLiteral("appointment-new")),
                                               QStringLiteral("New Event"), this);
        connect(m_calendarNewEventAction, &QAction::triggered, this, [invokeCalendar]
                { invokeCalendar(&javelin::gui::calendar::MonthCalendarWidget::createEvent); });
        actionCollection()->addAction(QStringLiteral("calendar_new_event"),
                                      m_calendarNewEventAction);
        m_calendarPreviousMonthAction = new QAction(QIcon::fromTheme(QStringLiteral("go-previous")),
                                                    QStringLiteral("Previous Month"), this);
        connect(
            m_calendarPreviousMonthAction, &QAction::triggered, this, [invokeCalendar]
            { invokeCalendar(&javelin::gui::calendar::MonthCalendarWidget::showPreviousMonth); });
        actionCollection()->addAction(QStringLiteral("calendar_previous_month"),
                                      m_calendarPreviousMonthAction);
        m_calendarTodayAction = new QAction(QIcon::fromTheme(QStringLiteral("go-jump-today")),
                                            QStringLiteral("Today"), this);
        connect(m_calendarTodayAction, &QAction::triggered, this, [invokeCalendar]
                { invokeCalendar(&javelin::gui::calendar::MonthCalendarWidget::showToday); });
        actionCollection()->addAction(QStringLiteral("calendar_today"), m_calendarTodayAction);
        m_calendarNextMonthAction = new QAction(QIcon::fromTheme(QStringLiteral("go-next")),
                                                QStringLiteral("Next Month"), this);
        connect(m_calendarNextMonthAction, &QAction::triggered, this, [invokeCalendar]
                { invokeCalendar(&javelin::gui::calendar::MonthCalendarWidget::showNextMonth); });
        actionCollection()->addAction(QStringLiteral("calendar_next_month"),
                                      m_calendarNextMonthAction);
        m_calendarListAction = new QAction(QIcon::fromTheme(QStringLiteral("view-calendar-list")),
                                           QStringLiteral("Calendars"), this);
        actionCollection()->addAction(QStringLiteral("calendar_list"), m_calendarListAction);
        m_calendarRefreshAction = new QAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                              QStringLiteral("Refresh Calendar"), this);
        connect(m_calendarRefreshAction, &QAction::triggered, this,
                [this] { refreshActiveTabFromServer(); });
        actionCollection()->addAction(QStringLiteral("calendar_refresh"), m_calendarRefreshAction);

        auto* logAction = new QAction(QIcon::fromTheme(QStringLiteral("view-list-text")),
                                      QStringLiteral("Application Log"), this);
        connect(logAction, &QAction::triggered, this,
                [this]
                {
                    auto* dialog = new javelin::gui::logging::LogViewerDialog(this);
                    dialog->setAttribute(Qt::WA_DeleteOnClose);
                    dialog->show();
                });
        actionCollection()->addAction(QStringLiteral("open_application_log"), logAction);
    }

    void MainWindow::setupUi()
    {
        setWindowTitle(QStringLiteral("Javelin Mail"));

        m_mailboxModel = new javelin::gui::mailboxes::MailboxTreeModel(m_accountRepository,
                                                                       m_queryService, this);
        m_messageModel = new javelin::gui::messages::MessageListModel(m_queryService, this);

        m_mailboxSearchEdit = new QLineEdit(this);
        m_mailboxSearchEdit->setClearButtonEnabled(true);
        m_mailboxSearchEdit->setPlaceholderText(
            QStringLiteral("Search this account on the server"));

        m_tabBar = new QTabBar(this);
        m_tabBar->setDocumentMode(true);
        m_tabBar->setTabsClosable(true);
        m_tabBar->setExpanding(false);
        m_tabBar->setElideMode(Qt::ElideRight);
        m_tabBar->setUsesScrollButtons(true);
        m_tabBar->setStyleSheet(
            QStringLiteral("QTabBar::tab { max-width: 220px; min-width: 120px; }"));
        m_tabBar->hide();
        m_tabBarPresenter =
            new TabBarPresenter(*m_tabBar, *this, m_accountRepository, m_queryService, this);

        m_mailboxView = new javelin::gui::mailboxes::MailboxTreeView(this);
        m_mailboxView->setModel(m_mailboxModel);
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

        m_messageCommandController =
            new MessageCommandController(m_mailService, m_queryService, *m_messageView, this, this);
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
        connect(m_messageCommandController, &MessageCommandController::submitRequested, this,
                [this](const QString& accountId)
                { submitQueuedEmailMutations(accountId.toStdString()); });

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
                            QStringLiteral("Messages can only be moved within their account."),
                            5000);
                        return;
                    }

                    auto selection = m_messageCommandController->selectedActionItems();
                    m_messageCommandController->queueTransfer(
                        sourceAccountId.toStdString(), *sourceMailboxId,
                        destinationMailboxId.toStdString(), std::move(selection),
                        MessageTransferOperation::Move, QStringLiteral("Queued move."));
                });

        auto* messagePane = new QWidget(this);
        auto* messageLayout = new QVBoxLayout(messagePane);
        messageLayout->setContentsMargins(0, 0, 0, 0);
        messageLayout->setSpacing(8);
        auto* messageHeader = new QWidget(messagePane);
        auto* messageHeaderLayout = new QHBoxLayout(messageHeader);
        messageHeaderLayout->setContentsMargins(8, 3, 8, 3);
        messageHeaderLayout->setSpacing(8);
        m_messageListTitleLabel = new ElidingLabel(messageHeader);
        m_messageListMetaLabel = new QLabel(messageHeader);
        m_searchServerButton = new QToolButton(messageHeader);
        m_searchServerButton->setText(QStringLiteral("Search server"));
        m_searchServerButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_searchServerButton->setToolTip(
            QStringLiteral("Replace results indexed on this device with authoritative server "
                           "results"));
        m_searchServerButton->setVisible(false);
        m_messageSortButton = new QToolButton(messageHeader);
        m_messageSortButton->setIcon(javelin::gui::themedSvgIcon(
            QStringLiteral(":/icons/thunderbird-icons/display-options.svg"),
            palette().color(QPalette::Text)));
        m_messageSortButton->setToolTip(QStringLiteral("Sort messages"));
        m_firstPageButton = new QToolButton(messageHeader);
        m_firstPageButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
        m_firstPageButton->setToolTip(QStringLiteral("First page"));
        m_previousPageButton = new QToolButton(messageHeader);
        m_previousPageButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/nav-left.svg"),
                                        palette().color(QPalette::Text)));
        m_previousPageButton->setToolTip(QStringLiteral("Previous page"));
        m_nextPageButton = new QToolButton(messageHeader);
        m_nextPageButton->setIcon(
            javelin::gui::themedSvgIcon(QStringLiteral(":/icons/thunderbird-icons/nav-right.svg"),
                                        palette().color(QPalette::Text)));
        m_nextPageButton->setToolTip(QStringLiteral("Next page"));
        m_lastPageButton = new QToolButton(messageHeader);
        m_lastPageButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
        m_lastPageButton->setToolTip(QStringLiteral("Last page"));
        m_pageNumberSpinBox = new QSpinBox(messageHeader);
        m_pageNumberSpinBox->setPrefix(QStringLiteral("Page "));
        m_pageNumberSpinBox->setAlignment(Qt::AlignCenter);
        m_pageNumberSpinBox->setToolTip(QStringLiteral("Enter a page number"));
        m_messagePageLabel = new QLabel(messageHeader);
        auto titleFont = m_messageListTitleLabel->font();
        titleFont.setPointSize(titleFont.pointSize() + 4);
        titleFont.setBold(true);
        m_messageListTitleLabel->setFont(titleFont);
        messageHeaderLayout->addWidget(m_messageListTitleLabel, 1);
        messageHeaderLayout->addWidget(m_messageListMetaLabel);
        messageHeaderLayout->addWidget(m_searchServerButton);
        messageHeaderLayout->addWidget(m_firstPageButton);
        messageHeaderLayout->addWidget(m_previousPageButton);
        messageHeaderLayout->addWidget(m_pageNumberSpinBox);
        messageHeaderLayout->addWidget(m_messagePageLabel);
        messageHeaderLayout->addWidget(m_nextPageButton);
        messageHeaderLayout->addWidget(m_lastPageButton);
        messageHeaderLayout->addWidget(m_messageSortButton);
        m_messageEmptyState = new QLabel(
            QStringLiteral("No messages are available for the selected mailbox yet."), messagePane);
        m_messageEmptyState->setWordWrap(true);
        messageLayout->addWidget(messageHeader);
        messageLayout->addWidget(m_messageEmptyState);
        messageLayout->addWidget(m_messageView, 1);

        m_messageViewContainer = new javelin::gui::messageview::MessageViewContainer(
            m_translationService, m_contactIdentityLookup, this);
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
                &MainWindow::selectMessageAlone);
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
                if (isExpanded && currentThreadId(*m_messageView) ==
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
                *m_messageEmptyState, *m_messageView, *m_searchServerButton, *m_firstPageButton,
                *m_previousPageButton, *m_pageNumberSpinBox, *m_nextPageButton, *m_lastPageButton,
                pageSize);
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
                    m_messageNavigationCoordinator.cancel();
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
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        const bool allowSearchSelection = activeTabIsSearch() && accountId.has_value();
        if (!accountId.has_value() || (!mailboxId.has_value() && !allowSearchSelection))
        {
            m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId,
                                                 std::nullopt);
            updateMessageActions();
            return;
        }

        if (!current.isValid())
        {
            m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId,
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
            QTimer::singleShot(
                0, this,
                [this, threadId = threadId.toStdString()]
                {
                    if (currentThreadId(*m_messageView) == std::optional<std::string>{threadId})
                    {
                        static_cast<void>(m_messageModel->setThreadExpanded(threadId, true));
                    }
                });
        }
        m_messageViewContainer->setSelection(
            m_messageViewService, accountId, mailboxId,
            emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                              : std::optional<std::string>{emailId.toStdString()});
        updateEmptyStates();
        updateMessageListHeader();
        updateMessageActions();
        if (!emailId.isEmpty())
        {
            if (!m_messageViewContainer->hasReadableBody())
            {
                m_messageViewContainer->setLoadingState(true);
            }
            refreshSelectedMessageContent(*accountId, emailId.toStdString());
            if (isUnread)
            {
                m_messageCommandController->markEmailRead(*accountId, emailId.toStdString());
            }
        }

        syncActiveTabSelectionFromViews();
    }

    void MainWindow::openContacts()
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (std::holds_alternative<ContactsTabState>(m_tabs[index].content))
            {
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, false);
                return;
            }
        }

        const auto result = m_contactRepository.listAccounts();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&result);
        if (accounts == nullptr || accounts->empty())
        {
            m_statusBar->showMessage(
                QStringLiteral("The configured server does not support JMAP Contacts."), 10000);
            return;
        }
        const auto active = activeAccountId();
        auto selected = accounts->begin();
        if (active.has_value())
        {
            const auto match = std::ranges::find(*accounts, *active,
                                                 &javelin::jmap::cache::ContactAccount::accountId);
            if (match != accounts->end())
            {
                selected = match;
            }
        }
        auto* widget = appendContactsTab(selected->ownerAccountId, QStringLiteral("Contacts"));
        if (widget == nullptr)
            return;
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
        widget->requestRefresh();
    }

    javelin::gui::contacts::ContactsManagerWidget*
    MainWindow::appendContactsTab(std::string ownerAccountId, QString title)
    {
        const auto availableAccounts = m_contactRepository.listAccounts(ownerAccountId);
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&availableAccounts);
        if (accounts == nullptr || accounts->empty())
            return nullptr;
        auto* widget = new javelin::gui::contacts::ContactsManagerWidget(
            m_contactRepository, m_mailService, ownerAccountId, m_contentStack);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::statusMessageRequested,
                m_statusBar, &LayeredStatusBar::showMessage);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::userInterventionRequired,
                this, &MainWindow::presentUserInterventionError);
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::composeMailRequested, this,
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
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::searchMailFromRequested,
                this,
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
        connect(widget, &javelin::gui::contacts::ContactsManagerWidget::toolbarStateChanged, this,
                [this, widget]
                {
                    const auto* tab = activeTab();
                    const auto* contacts =
                        tab == nullptr ? nullptr : std::get_if<ContactsTabState>(&tab->content);
                    if (contacts != nullptr && contacts->widget == widget)
                        updateToolbarForActiveTab();
                });
        m_contentStack->addWidget(widget);
        m_tabs.push_back(
            TabState{.content = ContactsTabState{.accountId = std::move(ownerAccountId),
                                                 .title = std::move(title),
                                                 .widget = widget,
                                                 .selection = {}}});
        return widget;
    }

    void MainWindow::openSieveEditor()
    {
        const auto accountId = activeAccountId();
        if (!accountId)
        {
            m_statusBar->showMessage(QStringLiteral("Select an account to edit its Sieve rules."),
                                     5000);
            return;
        }
        javelin::gui::sieve::SieveEditorDialog dialog{m_mailService, *accountId, this};
        dialog.exec();
    }

    void MainWindow::openCalendar()
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (std::holds_alternative<CalendarTabState>(m_tabs[index].content))
            {
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, false);
                return;
            }
        }

        const auto accountsResult = m_calendarService.accounts();
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&accountsResult))
        {
            presentError(*error);
            return;
        }
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&accountsResult);
        if (accounts == nullptr || accounts->empty())
        {
            m_statusBar->showMessage(
                QStringLiteral("The configured server does not support JMAP Calendars draft-26."),
                10000);
            return;
        }

        auto* widget = new javelin::gui::calendar::MonthCalendarWidget(m_contentStack);
        const auto loadVisible =
            [this, widget, accounts = *accounts](const QDate& start, const QDate& end)
        {
            std::vector<javelin::gui::calendar::MonthEvent> displayEvents;
            std::vector<javelin::gui::calendar::CalendarDisplay> calendarDisplays;
            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};
            for (const auto& account : accounts)
            {
                const auto listed = m_calendarService.calendars(account.accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&listed);
                const auto loaded =
                    m_calendarService.loadCached(account.accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                auto presentation = javelin::gui::calendar::buildCalendarAccountPresentation(
                    account,
                    calendars != nullptr ? *calendars
                                         : std::vector<javelin::jmap::calendar::Calendar>{},
                    window != nullptr ? *window
                                      : std::optional<javelin::jmap::cache::CalendarWindow>{},
                    widget->palette().color(QPalette::Highlight));
                calendarDisplays.append_range(std::move(presentation.calendars));
                displayEvents.append_range(std::move(presentation.events));
            }
            widget->setCalendars(std::move(calendarDisplays));
            widget->setEvents(std::move(displayEvents));
        };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, loadVisible);
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::calendarVisibilityChanged,
                widget,
                [this, loadVisible, widget](const QString& displayId, const bool visible)
                {
                    const auto separator = displayId.indexOf(QLatin1Char('\n'));
                    if (separator <= 0 || separator == displayId.size() - 1)
                        return;
                    const auto result = m_calendarService.setCalendarVisible(
                        displayId.first(separator).toStdString(),
                        displayId.sliced(separator + 1).toStdString(), visible);
                    if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                    {
                        presentError(*error);
                        loadVisible(widget->visibleStart(), widget->visibleEnd());
                    }
                });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::defaultCalendarChanged, widget,
            [this, accounts = *accounts, loadVisible, widget](const QString& displayId)
            {
                const auto separator = displayId.indexOf(QLatin1Char('\n'));
                if (separator <= 0 || separator == displayId.size() - 1)
                    return;
                const auto accountId = displayId.first(separator).toStdString();
                const auto account = std::ranges::find(
                    accounts, accountId, &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == accounts.end())
                    return;
                auto task =
                    m_mailService.setDefaultCalendar(account->ownerAccountId, accountId,
                                                     displayId.sliced(separator + 1).toStdString());
                QCoro::connect(std::move(task), widget,
                               [this, loadVisible,
                                widget](javelin::jmap::calendar::CalendarMutationResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                   {
                                       presentError(*error);
                                       loadVisible(widget->visibleStart(), widget->visibleEnd());
                                   }
                               });
            });
        connect(&m_mailService, &javelin::app::MailApplicationService::calendarCacheCommitted,
                widget,
                [widget, accounts = *accounts,
                 loadVisible](const javelin::app::CalendarCacheChange& change)
                {
                    const auto owner = change.ownerAccountId.toStdString();
                    if (std::ranges::none_of(accounts, [&owner](const auto& account)
                                             { return account.ownerAccountId == owner; }))
                        return;
                    loadVisible(widget->visibleStart(), widget->visibleEnd());
                });
        loadVisible(widget->visibleStart(), widget->visibleEnd());
        const auto refreshVisible =
            [this, widget, accounts = *accounts](const QDate& start, const QDate& end)
        {
            const javelin::jmap::calendar::VisibleInterval interval{
                .start = {.value = start.toString(Qt::ISODate).toStdString() + "T00:00:00"},
                .end = {.value = end.toString(Qt::ISODate).toStdString() + "T00:00:00"}};
            const javelin::jmap::calendar::TimeZoneId timeZone{
                .value = QTimeZone::systemTimeZoneId().toStdString()};
            std::unordered_set<std::string> owners;
            for (const auto& account : accounts)
            {
                if (!owners.insert(account.ownerAccountId).second)
                    continue;
                auto task =
                    m_mailService.requestCalendarRange(account.ownerAccountId, interval, timeZone);
                QCoro::connect(std::move(task), widget,
                               [this](javelin::jmap::calendar::CalendarRefreshResult result)
                               {
                                   if (const auto* error =
                                           std::get_if<javelin::jmap::OperationError>(&result))
                                   {
                                       presentError(*error);
                                       return;
                                   }
                               });
            }
        };
        connect(widget, &javelin::gui::calendar::MonthCalendarWidget::visibleIntervalChanged,
                widget, refreshVisible);
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::emptyTimeActivated, widget,
            [this, widget, accounts = *accounts, refreshVisible](const QDate& date)
            {
                std::vector<javelin::jmap::calendar::Calendar> choices;
                std::optional<std::size_t> destinationIndex;
                for (const auto& account : accounts)
                {
                    const auto calendarsResult = m_calendarService.calendars(account.accountId);
                    const auto* calendars =
                        std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(
                            &calendarsResult);
                    if (calendars == nullptr)
                        continue;
                    for (const auto& calendar : *calendars)
                    {
                        auto choice = calendar;
                        if (accounts.size() > 1)
                            choice.name += QStringLiteral(" — %1")
                                               .arg(QString::fromStdString(account.name))
                                               .toStdString();
                        const auto writable =
                            choice.myRights.mayWriteAll || choice.myRights.mayWriteOwn;
                        if (writable && (!destinationIndex.has_value() || choice.isDefault))
                            destinationIndex = choices.size();
                        choices.push_back(std::move(choice));
                    }
                }
                if (!destinationIndex.has_value())
                {
                    m_statusBar->showMessage(QStringLiteral("No writable calendar is available."),
                                             5000);
                    return;
                }
                const auto& destination = choices[*destinationIndex];
                auto* dialog = new javelin::gui::calendar::EventDialog(choices, widget);
                dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                dialog->setEvent(javelin::jmap::calendar::CalendarEvent{
                    .accountId = destination.accountId,
                    .id = {},
                    .baseEventId = std::nullopt,
                    .recurrenceId = std::nullopt,
                    .uid = {},
                    .calendarIds = {{destination.id, true}},
                    .title = {},
                    .description = std::nullopt,
                    .location = std::nullopt,
                    .start = {.value =
                                  QDateTime{date, QTime{9, 0}}.toString(Qt::ISODate).toStdString()},
                    .duration = {.value = "PT1H"},
                    .timeZone =
                        javelin::jmap::calendar::TimeZoneId{
                            .value = QTimeZone::systemTimeZoneId().toStdString()},
                    .showWithoutTime = false,
                    .isDraft = false,
                    .isOrigin = true,
                    .useDefaultAlerts = false,
                    .alerts = {},
                    .utcStart = std::nullopt,
                    .utcEnd = std::nullopt,
                    .recurrenceRule = std::nullopt,
                    .recurrenceOverrides = {},
                    .attendees = {}});
                if (dialog->exec() != QDialog::Accepted)
                {
                    dialog->deleteLater();
                    return;
                }
                auto event = dialog->eventDocument();
                const auto selectedAccount = std::ranges::find(
                    accounts, event.accountId, &javelin::jmap::cache::CalendarAccount::accountId);
                if (selectedAccount == accounts.end())
                {
                    dialog->showMutationError(
                        QStringLiteral("The selected calendar account is no longer available."));
                    dialog->show();
                    return;
                }
                auto task = m_mailService.createCalendarEvent(selectedAccount->ownerAccountId,
                                                              {.accountId = event.accountId,
                                                               .event = std::move(event),
                                                               .ifInState = std::nullopt});
                QCoro::connect(
                    std::move(task), dialog,
                    [dialog, widget,
                     refreshVisible](javelin::jmap::calendar::CalendarMutationResult result)
                    {
                        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        {
                            qCWarning(logUserOperations).noquote()
                                << "calendar event creation failed" << error->message;
                            dialog->showMutationError(error->message);
                            dialog->show();
                            return;
                        }
                        dialog->deleteLater();
                        refreshVisible(widget->visibleStart(), widget->visibleEnd());
                    });
            });
        connect(
            widget, &javelin::gui::calendar::MonthCalendarWidget::eventActivated, widget,
            [this, widget, accounts = *accounts, refreshVisible](
                const QString& accountId, const QString& eventId, const QString& recurrenceId)
            {
                const auto account =
                    std::ranges::find(accounts, accountId.toStdString(),
                                      &javelin::jmap::cache::CalendarAccount::accountId);
                if (account == accounts.end())
                    return;
                const javelin::jmap::calendar::VisibleInterval interval{
                    .start = {.value = widget->visibleStart().toString(Qt::ISODate).toStdString() +
                                       "T00:00:00"},
                    .end = {.value = widget->visibleEnd().toString(Qt::ISODate).toStdString() +
                                     "T00:00:00"}};
                const javelin::jmap::calendar::TimeZoneId timeZone{
                    .value = QTimeZone::systemTimeZoneId().toStdString()};
                const auto loaded =
                    m_calendarService.loadCached(account->accountId, interval, timeZone);
                const auto* window =
                    std::get_if<std::optional<javelin::jmap::cache::CalendarWindow>>(&loaded);
                if (window == nullptr || !window->has_value())
                    return;
                const auto event = std::ranges::find(window->value().events, eventId.toStdString(),
                                                     &javelin::jmap::calendar::CalendarEvent::id);
                if (event == window->value().events.end())
                    return;
                const auto calendarsResult = m_calendarService.calendars(account->accountId);
                const auto* calendars =
                    std::get_if<std::vector<javelin::jmap::calendar::Calendar>>(&calendarsResult);
                if (calendars == nullptr)
                    return;

                enum class EditScope
                {
                    Occurrence,
                    Series,
                };
                auto editScope = EditScope::Series;
                if (!recurrenceId.isEmpty())
                {
                    QMessageBox scopePrompt{widget};
                    scopePrompt.setWindowTitle(QStringLiteral("Edit recurring event"));
                    scopePrompt.setText(
                        QStringLiteral("Do you want to edit only this occurrence or the entire "
                                       "series?"));
                    auto* occurrenceButton = scopePrompt.addButton(
                        QStringLiteral("This occurrence"), QMessageBox::AcceptRole);
                    auto* seriesButton = scopePrompt.addButton(QStringLiteral("Entire series"),
                                                               QMessageBox::ActionRole);
                    scopePrompt.addButton(QMessageBox::Cancel);
                    scopePrompt.exec();
                    if (scopePrompt.clickedButton() == occurrenceButton)
                        editScope = EditScope::Occurrence;
                    else if (scopePrompt.clickedButton() != seriesButton)
                        return;
                }

                auto editableEvent = *event;
                if (editScope == EditScope::Occurrence)
                {
                    const auto occurrence = std::ranges::find_if(
                        window->value().occurrences,
                        [&eventId, &recurrenceId](const auto& candidate)
                        {
                            return candidate.eventId == eventId.toStdString() &&
                                   candidate.recurrenceId &&
                                   candidate.recurrenceId->value == recurrenceId.toStdString();
                        });
                    if (occurrence == window->value().occurrences.end())
                    {
                        qCWarning(logUserOperations).noquote()
                            << "calendar occurrence is missing from the visible cache" << accountId
                            << eventId << recurrenceId;
                        m_statusBar->showMessage(
                            QStringLiteral("This occurrence is no longer available. Refresh and "
                                           "try again."),
                            10000);
                        return;
                    }
                    editableEvent.start = occurrence->localStart;
                    const auto occurrenceStart = QDateTime::fromString(
                        QString::fromStdString(occurrence->localStart.value), Qt::ISODate);
                    const auto occurrenceEnd = QDateTime::fromString(
                        QString::fromStdString(occurrence->localEnd.value), Qt::ISODate);
                    if (occurrence->allDay)
                        editableEvent.duration.value =
                            QStringLiteral("P%1D")
                                .arg(occurrenceStart.date().daysTo(occurrenceEnd.date()))
                                .toStdString();
                    else
                        editableEvent.duration.value =
                            QStringLiteral("PT%1S")
                                .arg(occurrenceStart.secsTo(occurrenceEnd))
                                .toStdString();
                    if (const auto existingOverride =
                            event->recurrenceOverrides.find(recurrenceId.toStdString());
                        existingOverride != event->recurrenceOverrides.end() &&
                        existingOverride->second.title)
                        editableEvent.title = *existingOverride->second.title;
                }

                auto* dialog = new javelin::gui::calendar::EventDialog(*calendars, widget);
                dialog->setAttribute(Qt::WA_DeleteOnClose, false);
                dialog->setEvent(editableEvent);
                dialog->setOccurrenceMode(editScope == EditScope::Occurrence);
                const auto baseEvent = *event;
                const auto originalCalendarIds = event->calendarIds;
                const auto dialogResult = dialog->exec();
                if (dialogResult == QDialog::Rejected)
                {
                    dialog->deleteLater();
                    return;
                }
                auto editedEvent = dialog->eventDocument();
                const auto occurrenceEdit = editScope == EditScope::Occurrence;
                if (occurrenceEdit)
                {
                    const javelin::jmap::calendar::LocalDateTime selectedRecurrence{
                        .value = recurrenceId.toStdString()};
                    editedEvent =
                        dialogResult == javelin::gui::calendar::EventDialog::DeleteRequested
                            ? javelin::jmap::calendar::excludeOccurrence(baseEvent,
                                                                         selectedRecurrence)
                            : javelin::jmap::calendar::applyOccurrenceEdit(
                                  baseEvent, selectedRecurrence, editedEvent);
                }
                auto task =
                    dialogResult == javelin::gui::calendar::EventDialog::DeleteRequested &&
                            !occurrenceEdit
                        ? m_mailService.deleteCalendarEvent(
                              account->ownerAccountId,
                              {.accountId = account->accountId,
                               .eventId = editedEvent.id,
                               .calendarIds =
                                   [&originalCalendarIds]
                               {
                                   std::vector<std::string> ids;
                                   for (const auto& [calendarId, present] : originalCalendarIds)
                                   {
                                       if (present)
                                       {
                                           ids.push_back(calendarId);
                                       }
                                   }
                                   return ids;
                               }(),
                               .ifInState = std::nullopt})
                        : m_mailService.updateCalendarEvent(account->ownerAccountId,
                                                            {.accountId = account->accountId,
                                                             .event = editedEvent,
                                                             .ifInState = std::nullopt});
                QCoro::connect(
                    std::move(task), dialog,
                    [dialog, widget,
                     refreshVisible](javelin::jmap::calendar::CalendarMutationResult result)
                    {
                        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                        {
                            qCWarning(logUserOperations).noquote()
                                << "calendar event mutation failed" << error->message;
                            dialog->showMutationError(error->message);
                            dialog->show();
                            return;
                        }
                        dialog->deleteLater();
                        refreshVisible(widget->visibleStart(), widget->visibleEnd());
                    });
            });
        refreshVisible(widget->visibleStart(), widget->visibleEnd());
        m_contentStack->addWidget(widget);
        m_tabs.push_back(
            TabState{.content = CalendarTabState{.accountId = accounts->front().ownerAccountId,
                                                 .title = QStringLiteral("Calendar"),
                                                 .widget = widget,
                                                 .selection = {}}});
        m_activeTabIndex = static_cast<int>(m_tabs.size() - 1);
        updateTabBar();
        activateTab(*m_activeTabIndex, false);
    }

    void MainWindow::composeNewMessage()
    {
        const auto accountId =
            activeAccountId().has_value() ? activeAccountId() : currentAccountId(*m_mailboxView);
        if (!accountId.has_value())
        {
            m_statusBar->showMessage(
                QStringLiteral("Select an account before composing a message."), 5000);
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
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to reply to."), 5000);
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
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to reply to."), 5000);
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
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to forward."), 5000);
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
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a draft to edit."), 5000);
            return;
        }

        const auto draftsMailbox = findMailboxByRole(m_queryService, *accountId, "drafts");
        if (!activeMailboxId().has_value() || !draftsMailbox.has_value() ||
            draftsMailbox->id != *activeMailboxId())
        {
            m_statusBar->showMessage(QStringLiteral("Open a message from Drafts to edit it."),
                                     5000);
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
            bool busy = true;
            bool selected = false;
            if (const auto* tab = activeTab())
                if (const auto* contacts = std::get_if<ContactsTabState>(&tab->content);
                    contacts != nullptr && contacts->widget != nullptr)
                {
                    busy = contacts->widget->operationInFlight();
                    selected = contacts->widget->hasSelectedContact();
                }
            const auto* contacts = activeTab() == nullptr
                                       ? nullptr
                                       : std::get_if<ContactsTabState>(&activeTab()->content);
            const auto* widget = contacts == nullptr ? nullptr : contacts->widget;
            m_contactNewAction->setEnabled(!busy && widget != nullptr &&
                                           widget->canCreateContact());
            m_contactEditAction->setEnabled(!busy && selected && widget != nullptr &&
                                            widget->canEditContact());
            m_contactDeleteAction->setEnabled(!busy && selected && widget != nullptr &&
                                              widget->canDeleteContact());
            m_contactCopyAction->setEnabled(!busy && widget != nullptr &&
                                            widget->hasSingleSelectedContact());
            m_contactImportAction->setEnabled(!busy && widget != nullptr &&
                                              widget->canCreateContact());
            m_contactExportAction->setEnabled(!busy && widget != nullptr &&
                                              widget->hasSingleSelectedContact());
            m_contactDuplicatesAction->setEnabled(!busy);
            m_contactAddToGroupAction->setEnabled(
                !busy && widget != nullptr &&
                (widget->canCreateGroup() || widget->canAddSelectedContactToGroup()));
            m_contactRemoveFromGroupAction->setEnabled(!busy && widget != nullptr &&
                                                       widget->canRemoveSelectedContactFromGroup());
            m_contactManageAddressBooksAction->setEnabled(!busy);
            m_contactRefreshAction->setEnabled(!busy);
        }
        if (context == ToolbarContext::Calendar)
        {
            auto* menu = static_cast<QMenu*>(nullptr);
            if (const auto* tab = activeTab())
                if (const auto* calendar = std::get_if<CalendarTabState>(&tab->content);
                    calendar != nullptr && calendar->widget != nullptr)
                    menu = calendar->widget->calendarMenu();
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
        QElapsedTimer timer;
        timer.start();
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            const auto plan = planTabActivation({});
            m_activeTabIndex.reset();
            m_messageModel->clear();
            m_messageViewContainer->setSelection(m_messageViewService, std::nullopt, std::nullopt,
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
        if (const auto* composeTab = std::get_if<ComposeTabState>(&tab.content);
            composeTab != nullptr && composeTab->widget != nullptr)
        {
            m_contentStack->setCurrentWidget(composeTab->widget);
        }
        else if (const auto* contactsTab = std::get_if<ContactsTabState>(&tab.content);
                 contactsTab != nullptr && contactsTab->widget != nullptr)
        {
            m_contentStack->setCurrentWidget(contactsTab->widget);
        }
        else if (const auto* calendarTab = std::get_if<CalendarTabState>(&tab.content);
                 calendarTab != nullptr && calendarTab->widget != nullptr)
        {
            m_contentStack->setCurrentWidget(calendarTab->widget);
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
            refreshTabFromServer(static_cast<std::size_t>(index));

        qCDebug(logGuiMailbox).noquote() << "activate tab" << index << "refreshRemote"
                                         << refreshRemote << "ms" << timer.elapsed();
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

        if (std::holds_alternative<ComposeTabState>(
                m_tabs[static_cast<std::size_t>(index)].content) &&
            !closeComposeTab(index))
        {
            return;
        }

        if (auto* contactsTab =
                std::get_if<ContactsTabState>(&m_tabs[static_cast<std::size_t>(index)].content))
        {
            if (contactsTab->widget != nullptr && contactsTab->widget->operationInFlight())
            {
                m_statusBar->showMessage(
                    QStringLiteral("Wait for the Contacts operation to finish."), 5000);
                return;
            }
            if (contactsTab->widget != nullptr)
            {
                m_contentStack->removeWidget(contactsTab->widget);
                contactsTab->widget->deleteLater();
            }
        }

        if (auto* calendarTab =
                std::get_if<CalendarTabState>(&m_tabs[static_cast<std::size_t>(index)].content))
        {
            if (calendarTab->widget != nullptr)
            {
                m_contentStack->removeWidget(calendarTab->widget);
                calendarTab->widget->deleteLater();
            }
        }

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
        {
            QSignalBlocker messageSelectionBlocker{m_messageView->selectionModel()};
            m_messageListTabBindingPresenter->applyPage(activeTab());
            autoSelectedFallback = restoreActiveTabMessageSelection(previousMessageRow);
        }
        if (autoSelectedFallback)
            handleCurrentMessageChanged(m_messageView->currentIndex());
        else
            refreshSelectionFromModels();
    }

    void MainWindow::loadActiveTabFromCache(const bool forceReload, const bool refreshRemote)
    {
        auto* tab = activeTab();
        if (tab == nullptr)
        {
            m_messageListTabBindingPresenter->applyPage(nullptr);
            refreshSelectionFromModels();
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
                m_messageViewContainer->setSelection(m_messageViewService, std::nullopt,
                                                     std::nullopt, std::nullopt);
            }
            updateMessageActions();
            return;
        }

        applyActiveTabPagePreservingSelection(currentMessageRow(*m_messageView));
        if (refreshRemote &&
            (tabKind(*tab) == TabKind::Mailbox || m_messageListTabController->pageStale(*tab)))
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

        if (auto* contactsTab = std::get_if<ContactsTabState>(&tab.content);
            contactsTab != nullptr && contactsTab->widget != nullptr)
        {
            contactsTab->widget->requestRefresh();
        }
        else if (auto* calendarTab = std::get_if<CalendarTabState>(&tab.content);
                 calendarTab != nullptr && calendarTab->widget != nullptr)
        {
            calendarTab->widget->setDisplayedMonth(calendarTab->widget->displayedMonth());
        }
    }

    void MainWindow::activateMailboxSelection(const bool refreshRemote)
    {
        QElapsedTimer timer;
        timer.start();
        const auto accountId = currentAccountId(*m_mailboxView);
        const auto mailboxId = currentMailboxId(*m_mailboxView);
        if (!accountId.has_value() || !mailboxId.has_value())
        {
            return;
        }

        m_messageNavigationCoordinator.cancel();

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
        qInfo().noquote() << "GUI activate mailbox selection" << QString::fromStdString(*accountId)
                          << QString::fromStdString(*mailboxId) << "refreshRemote" << refreshRemote
                          << "ms" << timer.elapsed();
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
        const auto settings = javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
            QString::fromStdString(request.accountId));
        if (settings.sessionUrl.isEmpty() || settings.loginEmail.isEmpty() ||
            settings.apiKey.isEmpty())
        {
            presentUserInterventionError(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."));
            return;
        }

        auto task =
            m_composeService.open(toAccountConnectionSettings(settings), std::move(request));
        QCoro::connect(
            std::move(task), this,
            [this](std::variant<javelin::jmap::submission::DraftSnapshot,
                                javelin::jmap::OperationError>
                       result)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote() << "GUI compose open failed" << error->message;
                    presentError(*error);
                    return;
                }

                openOrActivateComposeTab(
                    std::get<javelin::jmap::submission::DraftSnapshot>(std::move(result)));
            });
    }

    void MainWindow::openOrActivateComposeTab(javelin::jmap::submission::DraftSnapshot snapshot)
    {
        for (std::size_t index = 0; index < m_tabs.size(); ++index)
        {
            if (auto* composeTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
                composeTab != nullptr && composeTab->composeSessionId == snapshot.composeSessionId)
            {
                composeTab->title = snapshot.subject.has_value()
                                        ? QString::fromStdString(*snapshot.subject)
                                        : composeTab->title;
                m_activeTabIndex = static_cast<int>(index);
                updateTabBar();
                activateTab(*m_activeTabIndex, false);
                return;
            }
        }

        m_tabs.push_back(TabState{
            .content =
                ComposeTabState{
                    .accountId = snapshot.accountId,
                    .composeSessionId = snapshot.composeSessionId,
                    .title = snapshot.subject.has_value()
                                 ? QString::fromStdString(*snapshot.subject)
                                 : QStringLiteral("Compose"),
                    .widget = nullptr,
                    .selection = {},
                },
        });
        const auto index = static_cast<int>(m_tabs.size() - 1);
        auto* widget = new javelin::gui::compose::ComposeTabWidget(
            m_composeService, m_identityRepository, m_contactIdentityLookup, std::move(snapshot),
            m_contentStack);
        attachComposeWidget(widget, index);
        m_activeTabIndex = index;
        updateTabBar();
        activateTab(index, false);
    }

    void MainWindow::attachComposeWidget(javelin::gui::compose::ComposeTabWidget* widget,
                                         const int tabIndex)
    {
        if (widget == nullptr || tabIndex < 0 ||
            static_cast<std::size_t>(tabIndex) >= m_tabs.size())
        {
            return;
        }

        auto* composeTab =
            std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(tabIndex)].content);
        if (composeTab == nullptr)
        {
            return;
        }

        composeTab->widget = widget;
        composeTab->title = widget->tabTitle();
        m_contentStack->addWidget(widget);
        connect(widget, &javelin::gui::compose::ComposeTabWidget::titleChanged, this,
                [this, widget](const QString& title)
                {
                    for (std::size_t index = 0; index < m_tabs.size(); ++index)
                    {
                        auto* matchingTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
                        if (matchingTab == nullptr || matchingTab->widget != widget)
                        {
                            continue;
                        }

                        matchingTab->title = title;
                        updateTabBar();
                        return;
                    }
                });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::accountChanged, this,
                [this, widget](const QString& accountId)
                {
                    for (auto& tab : m_tabs)
                    {
                        auto* matchingTab = std::get_if<ComposeTabState>(&tab.content);
                        if (matchingTab != nullptr && matchingTab->widget == widget)
                        {
                            matchingTab->accountId = accountId.toStdString();
                            return;
                        }
                    }
                });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::statusMessageRequested, this,
                [this](const QString& message, const int timeoutMs)
                { m_statusBar->showMessage(message, timeoutMs); });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::userInterventionRequired, this,
                [this](const QString& message)
                { QMessageBox::critical(this, QStringLiteral("Action Required"), message); });
        connect(widget, &javelin::gui::compose::ComposeTabWidget::closeRequested, this,
                [this, widget]
                {
                    for (std::size_t index = 0; index < m_tabs.size(); ++index)
                    {
                        auto* matchingTab = std::get_if<ComposeTabState>(&m_tabs[index].content);
                        if (matchingTab != nullptr && matchingTab->widget == widget)
                        {
                            closeTab(static_cast<int>(index));
                            return;
                        }
                    }
                });
    }

    bool MainWindow::closeComposeTab(const int index)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= m_tabs.size())
        {
            return false;
        }

        auto* composeTab =
            std::get_if<ComposeTabState>(&m_tabs[static_cast<std::size_t>(index)].content);
        if (composeTab == nullptr || composeTab->widget == nullptr)
        {
            return false;
        }

        auto* widget = composeTab->widget;
        if (widget->operationInFlight())
        {
            m_statusBar->showMessage(
                QStringLiteral("Wait for the current compose operation to finish first."), 5000);
            return false;
        }

        if (widget->closeWithoutPrompt())
        {
            m_contentStack->removeWidget(widget);
            widget->deleteLater();
            composeTab->widget = nullptr;
            return true;
        }

        if (widget->isEmptyDraft())
        {
            if (const auto error = m_composeService.discard(widget->composeSessionId()))
            {
                presentError(*error);
                return false;
            }
            m_contentStack->removeWidget(widget);
            widget->deleteLater();
            composeTab->widget = nullptr;
            return true;
        }

        if (widget->draftEmailId().has_value())
        {
            QMessageBox messageBox{this};
            messageBox.setWindowTitle(QStringLiteral("Close Draft"));
            messageBox.setText(QStringLiteral(
                "This message is already saved in Drafts. Close the tab and keep it there?"));
            QAbstractButton* keepDraftButton =
                messageBox.addButton(QStringLiteral("Keep Draft"), QMessageBox::AcceptRole);
            messageBox.addButton(QMessageBox::Cancel);
            messageBox.exec();
            if (messageBox.clickedButton() != keepDraftButton)
            {
                return false;
            }

            if (const auto error = m_composeService.discard(widget->composeSessionId()))
            {
                presentError(*error);
                return false;
            }
            m_contentStack->removeWidget(widget);
            widget->deleteLater();
            composeTab->widget = nullptr;
            return true;
        }

        QMessageBox messageBox{this};
        messageBox.setWindowTitle(QStringLiteral("Close Compose Tab"));
        messageBox.setText(QStringLiteral("Save this message as a draft before closing?"));
        QAbstractButton* saveButton =
            messageBox.addButton(QStringLiteral("Save Draft"), QMessageBox::AcceptRole);
        QAbstractButton* discardButton =
            messageBox.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        messageBox.addButton(QMessageBox::Cancel);
        messageBox.exec();
        if (messageBox.clickedButton() == saveButton)
        {
            widget->saveDraftAndClose();
            return false;
        }
        if (messageBox.clickedButton() != discardButton)
        {
            return false;
        }

        if (const auto error = m_composeService.discard(widget->composeSessionId()))
        {
            presentError(*error);
            return false;
        }

        m_contentStack->removeWidget(widget);
        widget->deleteLater();
        composeTab->widget = nullptr;
        return true;
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
            m_statusBar->showMessage(QStringLiteral("Select an account before searching."), 5000);
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
            m_statusBar->showMessage(QStringLiteral("Select an account before searching."), 5000);
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
            m_statusBar->showMessage(QStringLiteral("Enter at least one search field."), 5000);
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

        addPropertyAction(QStringLiteral("Date received"),
                          javelin::jmap::query::EmailListSortProperty::ReceivedAt);
        addPropertyAction(QStringLiteral("Date sent"),
                          javelin::jmap::query::EmailListSortProperty::SentAt);
        addPropertyAction(QStringLiteral("From"),
                          javelin::jmap::query::EmailListSortProperty::From);
        addPropertyAction(QStringLiteral("To"), javelin::jmap::query::EmailListSortProperty::To);
        addPropertyAction(QStringLiteral("Subject"),
                          javelin::jmap::query::EmailListSortProperty::Subject);
        addPropertyAction(QStringLiteral("Size"),
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

        addDirectionAction(QStringLiteral("Descending"),
                           javelin::jmap::query::EmailListSortDirection::Descending);
        addDirectionAction(QStringLiteral("Ascending"),
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
        saveEmailListSort(m_emailListSort);

        updateSortButton();
        loadActiveTabFromCache(true);
        updateMessageListHeader();
        m_statusBar->showMessage(QStringLiteral("Sorting by %1, %2.")
                                     .arg(sortPropertyLabel(m_emailListSort.property),
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

    void MainWindow::syncActiveTabSelectionFromViews()
    {
        if (auto* tab = activeTab())
        {
            auto selection = TabSelectionState{
                .threadId = currentThreadId(*m_messageView),
                .emailId = currentEmailId(*m_messageView),
                .selectedEmailIds = selectedEmailIds(),
            };
            tabSelection(*tab) = std::move(selection);
        }
    }

    void MainWindow::reloadAccounts()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        if (m_contactsAction != nullptr)
        {
            const auto result = m_contactRepository.listAccounts();
            const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::ContactAccount>>(&result);
            m_contactsAction->setEnabled(accounts != nullptr && !accounts->empty());
        }
        if (m_calendarAction != nullptr)
        {
            const auto result = m_calendarService.accounts();
            const auto* accounts =
                std::get_if<std::vector<javelin::jmap::cache::CalendarAccount>>(&result);
            m_calendarAction->setEnabled(accounts != nullptr && !accounts->empty());
        }
    }

    void MainWindow::refreshViewsFromCache()
    {
        m_mailboxModel->refresh();
        m_mailboxView->expandAll();
        loadActiveTabFromCache(true);
    }

    void MainWindow::refreshActiveSearchAfterMutation(const std::string_view accountId)
    {
        if (auto* tab = activeTab(); tab != nullptr)
            static_cast<void>(
                m_messageListTabController->refreshSearchAfterMutation(*tab, accountId));
    }

    void MainWindow::restoreSelection(std::optional<std::string> accountId,
                                      std::optional<std::string> mailboxId,
                                      std::optional<std::string> threadId,
                                      std::optional<std::string> emailId,
                                      const bool scrollToSelection)
    {
        if (accountId.has_value())
        {
            const auto mailboxIdValue =
                mailboxId.has_value() ? std::optional<QString>{QString::fromStdString(*mailboxId)}
                                      : std::optional<QString>{std::nullopt};
            const QModelIndex mailboxIndex = findMailboxIndexForSelection(
                *m_mailboxModel, QString::fromStdString(*accountId), mailboxIdValue);
            if (mailboxIndex.isValid())
            {
                m_mailboxView->setCurrentIndex(mailboxIndex);
                if (scrollToSelection)
                {
                    m_mailboxView->scrollTo(mailboxIndex);
                }
            }
        }

        const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
            messageRowIdentities(*m_messageModel), {
                                                       .threadId = std::move(threadId),
                                                       .emailId = std::move(emailId),
                                                       .selectedEmailIds = {},
                                                       .previousRow = std::nullopt,
                                                   });
        if (!plan.currentRow.has_value())
            return;

        const auto selectedMessageIndex =
            m_messageModel->index(static_cast<int>(*plan.currentRow), 0);
        if (!selectedMessageIndex.isValid())
            return;

        m_messageView->setCurrentIndex(selectedMessageIndex);
        if (scrollToSelection)
            m_messageView->scrollTo(selectedMessageIndex);
    }

    bool MainWindow::restoreActiveTabMessageSelection(const std::optional<int> previousMessageRow)
    {
        const auto* tab = activeTab();
        if (tab == nullptr)
            return false;

        const auto& selection = tabSelection(*tab);
        const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
            messageRowIdentities(*m_messageModel),
            {
                .threadId = selection.threadId,
                .emailId = selection.emailId,
                .selectedEmailIds = selection.selectedEmailIds,
                .previousRow =
                    previousMessageRow.has_value() && *previousMessageRow >= 0
                        ? std::optional<std::size_t>{static_cast<std::size_t>(*previousMessageRow)}
                        : std::nullopt,
            });

        auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr && !plan.selectedRows.empty())
        {
            QItemSelection restoredSelection;
            for (const auto row : plan.selectedRows)
            {
                const auto index = m_messageModel->index(static_cast<int>(row), 0);
                if (index.isValid())
                    restoredSelection.select(index, index);
            }
            selectionModel->select(restoredSelection,
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }

        if (!plan.currentRow.has_value())
            return false;

        const auto currentIndex = m_messageModel->index(static_cast<int>(*plan.currentRow), 0);
        if (!currentIndex.isValid())
            return false;

        if (selectionModel != nullptr && !plan.selectedRows.empty())
        {
            selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
        }
        else if (selectionModel != nullptr && (plan.currentEmailChanged || plan.fallbackSelected))
        {
            selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::ClearAndSelect |
                                                              QItemSelectionModel::Rows);
        }
        else
        {
            m_messageView->setCurrentIndex(currentIndex);
        }

        if (plan.fallbackSelected)
            m_messageView->scrollTo(currentIndex);
        return plan.currentEmailChanged || plan.fallbackSelected;
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
        auto selectedSummaries = selectedMessageSummaries();
        if (selectedSummaries.size() > 1)
        {
            m_messageViewContainer->setMultipleSelection(accountId, mailboxId,
                                                         std::move(selectedSummaries));
            syncActiveTabSelectionFromViews();
            updateEmptyStates();
            updateMessageListHeader();
            updateMessageActions();
            return;
        }

        const auto emailId = currentEmailId(*m_messageView);
        if (!emailId.has_value())
        {
            if (const auto* route = m_messageNavigationController->activeRoute(activeTab()))
            {
                m_messageViewContainer->setSelection(m_messageViewService, route->accountId,
                                                     route->mailboxId, route->emailId);
                updateEmptyStates();
                updateMessageListHeader();
                updateMessageActions();
                if (!m_messageViewContainer->hasReadableBody())
                {
                    m_messageViewContainer->setLoadingState(true);
                    refreshSelectedMessageContent(route->accountId, route->emailId);
                }
                return;
            }
        }
        m_messageViewContainer->setSelection(m_messageViewService, accountId, mailboxId, emailId);
        syncActiveTabSelectionFromViews();
        updateEmptyStates();
        updateMessageListHeader();
        updateMessageActions();

        if (accountId.has_value() && emailId.has_value() &&
            !m_messageViewContainer->hasReadableBody())
        {
            m_messageViewContainer->setLoadingState(true);
            refreshSelectedMessageContent(*accountId, *emailId);
        }
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
        const auto selectedIds = selectedEmailIds();
        const auto accountId = activeAccountId();
        const auto mailboxId = activeMailboxId();
        const auto draftsMailbox =
            accountId.has_value()
                ? findMailboxByRole(m_queryService, *accountId, "drafts")
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

        m_messageSortButton->setToolTip(QStringLiteral("Sort messages: %1, %2")
                                            .arg(sortPropertyLabel(m_emailListSort.property),
                                                 sortDirectionLabel(m_emailListSort.direction)));
    }

    bool MainWindow::eventFilter(QObject* watched, QEvent* event)
    {
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
                        restoreSelection(activeAccountId(), activeMailboxId(),
                                         std::optional<std::string>{threadId.toStdString()},
                                         summaryEmailId);
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

    void MainWindow::openPreferencesForConnection(const QString& connectionId)
    {
        javelin::gui::settings::PreferencesDialog dialog{m_accountRepository, m_queryService, this};
        if (!connectionId.isEmpty())
            dialog.selectConfiguredAccount(connectionId);
        if (dialog.exec() == QDialog::Accepted)
        {
            m_translationService.reloadSettings();
            m_messageViewContainer->translationSettingsChanged();
            m_statusBar->showMessage(QStringLiteral("Saved preferences."), 3000);
            Q_EMIT accountSettingsChanged();
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
            presentUserInterventionError(QStringLiteral("Select an account to refresh."));
            return;
        }
        refreshAccountFromServer(*accountId);
    }

    void MainWindow::refreshAccountFromServer(std::string accountId)
    {
        if (!m_mailService.requestAccountSynchronization(accountId))
        {
            refreshConnectionSettings(
                javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
                    QString::fromStdString(accountId)));
            return;
        }
        m_statusBar->showMessage(QStringLiteral("Synchronizing account..."));
    }

    void MainWindow::refreshConnectionSettings(javelin::gui::settings::ConnectionSettings settings)
    {
        if (m_refreshInFlight)
        {
            return;
        }

        if (settings.loginEmail.isEmpty() || settings.apiKey.isEmpty())
        {
            presentUserInterventionError(
                QStringLiteral("Set Session URL, Login Email, and API Key in Preferences first."));
            return;
        }

        m_refreshInFlight = true;
        m_refreshAction->setEnabled(false);
        m_preferencesAction->setEnabled(false);
        m_statusBar->showMessage(QStringLiteral("Refreshing mail from server..."));
        qInfo().noquote() << "GUI refresh requested" << settings.loginEmail << settings.sessionUrl;

        std::vector<std::string> mailboxIds;
        for (const auto& accountId : std::as_const(settings.cachedAccountIds))
        {
            const auto syncedMailboxIds =
                javelin::gui::settings::PreferencesDialog::syncedMailboxIds(accountId);
            for (const auto& mailboxId : syncedMailboxIds)
            {
                mailboxIds.push_back(mailboxId.toStdString());
            }
        }
        auto task = m_mailService.bootstrapAccount(javelin::app::AccountBootstrapIntent{
            .settings = toAccountConnectionSettings(settings),
            .mailboxIds = std::move(mailboxIds),
        });
        QCoro::connect(
            std::move(task), this,
            [this, settings](javelin::jmap::LiveRefreshResult result)
            {
                m_refreshInFlight = false;
                m_refreshAction->setEnabled(true);
                m_preferencesAction->setEnabled(true);

                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    qWarning().noquote() << "GUI refresh failed" << error->message;
                    presentError(*error);
                    return;
                }

                const auto& summary = std::get<javelin::jmap::LiveRefreshSummary>(result);
                javelin::gui::settings::PreferencesDialog::saveResolvedSessionUrl(
                    settings.id, QString::fromStdString(summary.resolvedSessionUrl));
                const auto ownedAccounts = m_accountRepository.listOwnedBy(summary.accountId);
                if (const auto* accounts =
                        std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(
                            &ownedAccounts))
                {
                    for (const auto& account : *accounts)
                    {
                        javelin::gui::settings::PreferencesDialog::associateCachedAccount(
                            settings.id, QString::fromStdString(account.accountId));
                    }
                }
                qInfo().noquote() << "GUI refresh succeeded"
                                  << QString::fromStdString(summary.accountId)
                                  << static_cast<qulonglong>(summary.mailboxCount)
                                  << static_cast<qulonglong>(summary.emailCount);
                if (summary.selectedMailboxId.has_value())
                {
                    markTabsStaleForAccount(summary.accountId,
                                            std::string_view{*summary.selectedMailboxId});
                }
                else
                {
                    markTabsStaleForAccount(summary.accountId);
                }
                reloadAccounts();
                refreshViewsFromCache();
                m_statusBar->showMessage(
                    QStringLiteral("Synced %1 mailboxes and %2 messages for %3.")
                        .arg(summary.mailboxCount)
                        .arg(summary.emailCount)
                        .arg(QString::fromStdString(summary.accountId)),
                    10000);
                Q_EMIT accountSettingsChanged();
                auto contactsTask = m_mailService.requestContacts(summary.accountId);
                QCoro::connect(
                    std::move(contactsTask), this,
                    [this](javelin::jmap::contacts::ContactRefreshResult contactsResult)
                    {
                        if (const auto* error =
                                std::get_if<javelin::jmap::OperationError>(&contactsResult))
                        {
                            qWarning().noquote() << "Contacts refresh failed" << error->message;
                            return;
                        }
                        reloadAccounts();
                        const auto& contacts =
                            std::get<javelin::jmap::contacts::ContactRefreshSummary>(
                                contactsResult);
                        qInfo() << "Contacts cache refreshed" << contacts.contactCount;
                    });
            });
    }

    void MainWindow::refreshSelectedMessageContent(std::string accountId, std::string emailId)
    {
        if (m_messageContentRequestInFlight.has_value() &&
            m_messageContentRequestInFlight->accountId == accountId &&
            m_messageContentRequestInFlight->emailId == emailId)
        {
            qDebug().noquote() << "GUI message content refresh already in flight"
                               << QString::fromStdString(emailId);
            return;
        }

        const auto requestToken = m_nextMessageContentRequestToken++;
        m_messageContentRequestInFlight = MessageContentRequestState{
            .accountId = accountId,
            .emailId = emailId,
            .token = requestToken,
        };

        auto task = m_mailService.requestMessageContent(accountId, emailId);
        QCoro::connect(
            std::move(task), this,
            [this, accountId = std::move(accountId), emailId = std::move(emailId),
             requestToken](javelin::jmap::MessageContentRefreshResult result)
            {
                const bool isCurrentRequest =
                    m_messageContentRequestInFlight.has_value() &&
                    m_messageContentRequestInFlight->token == requestToken &&
                    m_messageContentRequestInFlight->accountId == accountId &&
                    m_messageContentRequestInFlight->emailId == emailId;
                if (!isCurrentRequest)
                {
                    qDebug().noquote() << "GUI message content refresh ignored stale completion"
                                       << QString::fromStdString(emailId);
                    return;
                }

                m_messageContentRequestInFlight.reset();
                if (const auto* unavailable =
                        std::get_if<javelin::jmap::MessageContentUnavailable>(&result))
                {
                    markTabsStaleForAccount(accountId);
                    refreshActiveTabFromServer();
                    const QString message =
                        unavailable->message + QStringLiteral(" Refreshing the current view…");
                    m_messageViewContainer->setErrorState(message);
                    m_statusBar->showMessage(message, 10000);
                    qWarning().noquote()
                        << "GUI message content unavailable" << unavailable->message;
                    return;
                }

                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&result))
                {
                    m_messageViewContainer->setErrorState(error->message);
                    qWarning().noquote() << "GUI message content refresh failed" << error->message;
                    presentError(*error);
                    return;
                }

                const auto currentAccount = activeAccountId();
                const auto selectedEmails = selectedEmailIds();
                const auto* route = m_messageNavigationController->activeRoute(activeTab());
                const bool routeOwnsDetail =
                    route != nullptr && route->accountId == accountId && route->emailId == emailId;
                if (currentAccount != std::optional<std::string>{accountId} ||
                    (!routeOwnsDetail &&
                     (selectedEmails.size() != 1 || selectedEmails.front() != emailId)))
                {
                    return;
                }

                m_messageViewContainer->refresh(m_messageViewService);
                updateEmptyStates();
                updateMessageListHeader();
                updateMessageActions();

                const auto& summary = std::get<javelin::jmap::MessageContentRefreshSummary>(result);
                qInfo().noquote() << "GUI message content refresh succeeded"
                                  << QString::fromStdString(summary.emailId)
                                  << static_cast<qulonglong>(summary.partCount)
                                  << static_cast<qulonglong>(summary.bodyValueCount)
                                  << summary.usedCachedContent;
                if (!summary.usedCachedContent)
                {
                    m_statusBar->showMessage(QStringLiteral("Message ready."), 5000);
                }
            });
    }

    std::vector<std::string> MainWindow::selectedEmailIds() const
    {
        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seen;
        const auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList indexes = selectionModel->selectedRows();
            emailIds.reserve(static_cast<std::size_t>(indexes.size()));
            for (const auto& index : indexes)
            {
                const auto emailId =
                    index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                        .toString()
                        .toStdString();
                if (!emailId.empty() && seen.insert(emailId).second)
                {
                    emailIds.push_back(emailId);
                }
            }
        }

        if (emailIds.empty())
        {
            if (const auto emailId = currentEmailId(*m_messageView); emailId.has_value())
            {
                emailIds.push_back(*emailId);
            }
        }

        return emailIds;
    }

    std::vector<javelin::jmap::cache::MessageListItem> MainWindow::selectedMessageSummaries() const
    {
        std::vector<QModelIndex> indexes;
        const auto* selectionModel = m_messageView->selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList selectedRows = selectionModel->selectedRows();
            indexes.reserve(static_cast<std::size_t>(selectedRows.size()));
            for (const auto& index : selectedRows)
            {
                if (index.isValid())
                {
                    indexes.push_back(index);
                }
            }
        }

        std::ranges::sort(indexes, [](const QModelIndex& left, const QModelIndex& right)
                          { return left.row() < right.row(); });

        std::vector<javelin::jmap::cache::MessageListItem> summaries;
        summaries.reserve(indexes.size());
        std::unordered_set<std::string> seen;
        for (const auto& index : indexes)
        {
            auto emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                               .toString()
                               .toStdString();
            if (emailId.empty() || !seen.insert(emailId).second)
            {
                continue;
            }

            const auto subject =
                index.data(javelin::gui::messages::MessageListModel::SubjectRole).toString();
            const auto preview =
                index.data(javelin::gui::messages::MessageListModel::PreviewRole).toString();
            summaries.push_back(javelin::jmap::cache::MessageListItem{
                .emailId = std::move(emailId),
                .threadId = index.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                                .toString()
                                .toStdString(),
                .subject = subject.isEmpty() ? std::nullopt
                                             : std::optional<std::string>{subject.toStdString()},
                .preview = preview.isEmpty() ? std::nullopt
                                             : std::optional<std::string>{preview.toStdString()},
                .receivedAt = index.data(javelin::gui::messages::MessageListModel::ReceivedAtRole)
                                  .toString()
                                  .toStdString(),
                .sentAt = std::nullopt,
                .threadMessageCount =
                    index.data(javelin::gui::messages::MessageListModel::ThreadMessageCountRole)
                        .toULongLong(),
                .hasAttachment =
                    index.data(javelin::gui::messages::MessageListModel::HasAttachmentRole)
                        .toBool(),
                .isUnread =
                    index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool(),
                .isFlagged =
                    index.data(javelin::gui::messages::MessageListModel::IsFlaggedRole).toBool(),
                .from = std::nullopt,
                .mailboxNames = {},
            });
        }

        return summaries;
    }

    void MainWindow::selectMessageAlone(const QString& emailId)
    {
        if (emailId.isEmpty())
        {
            return;
        }

        const QModelIndex index = findIndexByRole(
            *m_messageModel, javelin::gui::messages::MessageListModel::EmailIdRole, emailId);
        if (!index.isValid())
        {
            return;
        }

        auto* selectionModel = m_messageView->selectionModel();
        selectionModel->select(index,
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_messageView->setCurrentIndex(index);
        m_messageView->scrollTo(index);
        refreshSelectionFromModels();
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
            m_statusBar->showMessage(QStringLiteral("No sender address is available."), 5000);
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
                menu.addAction(QStringLiteral("Find all conversations with %1").arg(senderEmail));
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
            auto* moveMenu = menu.addMenu(QStringLiteral("Move to"));
            auto* copyMenu = menu.addMenu(QStringLiteral("Copy to"));
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
            auto* refreshAccountAction = menu.addAction(QStringLiteral("Refresh Account"));
            connect(refreshAccountAction, &QAction::triggered, this,
                    [this, account = accountId.toStdString()]
                    { refreshAccountFromServer(account); });
            menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
            return;
        }
        auto* openAsTabAction = menu.addAction(QStringLiteral("Open as Tab"));
        connect(openAsTabAction, &QAction::triggered, this,
                [this] { openMailboxSelectionInTab(true); });
        menu.addSeparator();
        auto* propertiesAction = menu.addAction(QStringLiteral("Properties…"));
        connect(
            propertiesAction, &QAction::triggered, this,
            [this, accountId, mailboxId]
            {
                const auto result = m_queryService.listMailboxTree(accountId.toStdString());
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
                    m_statusBar->showMessage(QStringLiteral("The mailbox is no longer available."),
                                             5000);
                    return;
                }

                javelin::gui::mailboxes::MailboxPropertiesDialog dialog{accountId, *mailbox, this};
                dialog.exec();
            });
        menu.exec(m_mailboxView->viewport()->mapToGlobal(position));
    }

    void MainWindow::refreshMessageListPreservingSelection()
    {
        syncActiveTabSelectionFromViews();

        QSignalBlocker blocker{m_messageView->selectionModel()};
        loadActiveTabFromCache(true, false);
    }

    void MainWindow::submitQueuedEmailMutations(std::string accountId)
    {
        auto task = m_mailService.submitPendingEmailMutations(accountId);
        QCoro::connect(
            std::move(task), this,
            [this](javelin::jmap::SubmittedEmailMutationsResult submitResult)
            {
                if (const auto* error = std::get_if<javelin::jmap::OperationError>(&submitResult))
                {
                    presentError(*error);
                    return;
                }

                const auto& summary =
                    std::get<javelin::jmap::SubmittedEmailMutations>(submitResult);
                if (summary.failedEmailCount > 0)
                {
                    markTabsStaleForAccount(summary.accountId);
                    refreshActiveTabFromServer();
                    presentError(javelin::jmap::OperationError{
                        .message = QStringLiteral("The server rejected %1 email mutation(s). "
                                                  "The mailbox has been refreshed to restore "
                                                  "the server state.")
                                       .arg(summary.failedEmailCount),
                    });
                    return;
                }
                if (summary.updatedEmailCount == 0)
                {
                    return;
                }

                refreshMessageListPreservingSelection();
                refreshSelectionFromModels();
                refreshActiveSearchAfterMutation(summary.accountId);
            });
    }

    void MainWindow::viewSelectedMessageSource()
    {
        const auto accountId = activeAccountId();
        const auto emailId = currentEmailId(*m_messageView);
        if (!accountId.has_value() || !emailId.has_value())
        {
            m_statusBar->showMessage(QStringLiteral("Select a message to view its source."), 3000);
            return;
        }

        m_messageFileController->viewMessageSource(*accountId, *emailId);
    }

    void MainWindow::restorePersistentState()
    {
        auto state = loadMainWindowState(m_emailListSort);
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
                    {
                        openCalendar();
                        if (!m_tabs.empty() && persisted.displayedMonth.isValid())
                        {
                            if (auto* calendar =
                                    std::get_if<CalendarTabState>(&m_tabs.back().content);
                                calendar != nullptr && calendar->widget != nullptr)
                                calendar->widget->setDisplayedMonth(persisted.displayedMonth);
                        }
                    }
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
        const auto draftResult = m_composeService.loadWorkingCopy(tab.composeSessionId);
        if (const auto* error = std::get_if<javelin::jmap::OperationError>(&draftResult))
        {
            presentError(*error);
            return;
        }

        const auto& snapshot =
            std::get<std::optional<javelin::jmap::submission::DraftSnapshot>>(draftResult);
        if (!snapshot.has_value())
        {
            return;
        }

        m_tabs.push_back(TabState{
            .content =
                ComposeTabState{
                    .accountId = snapshot->accountId,
                    .composeSessionId = snapshot->composeSessionId,
                    .title =
                        tab.common.title.isEmpty() ? QStringLiteral("Compose") : tab.common.title,
                    .widget = nullptr,
                    .selection = {},
                },
        });
        auto* widget = new javelin::gui::compose::ComposeTabWidget(
            m_composeService, m_identityRepository, m_contactIdentityLookup, *snapshot,
            m_contentStack);
        attachComposeWidget(widget, static_cast<int>(m_tabs.size() - 1));
    }

    void MainWindow::restoreContactsTab(const PersistedContactsTab& tab)
    {
        auto* widget = appendContactsTab(tab.common.accountId, tab.common.title.isEmpty()
                                                                   ? QStringLiteral("Contacts")
                                                                   : tab.common.title);
        if (widget == nullptr)
            return;
        widget->restoreViewState(tab.view);
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
        saveMainWindowState(state);
    }

    void MainWindow::closeEvent(QCloseEvent* event)
    {
        savePersistentState();
        KXmlGuiWindow::closeEvent(event);
    }

} // namespace javelin::gui::shell
