#pragma once

#include "gui/shell/TabWorkspace.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/submission/ComposeTypes.h"

#include <KXmlGuiWindow>
#include <QModelIndex>
#include <QPointer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QListView;
class QPoint;
class QProgressBar;
class QMenu;
class QSplitter;
class QStackedWidget;
class QTabBar;
class QTimer;
class QToolButton;
class QTreeView;
class QAction;
class QWidget;

namespace javelin::jmap
{
    struct OperationError;
} // namespace javelin::jmap

namespace javelin::app
{
    enum class MailAccountStatus;
    class AccountCommandPort;
    class DeveloperDiagnosticsPort;
    class DeveloperMaintenancePort;
    class DaemonLogPort;
    class MailCommandPort;
    class MailExportPort;
    class SieveCommandPort;
    class IdentityCommandPort;
    class AccountRefreshPort;
    class MessageContentPort;
    class MessageListSessionFactoryPort;
    class MailApplicationEventsPort;
    class UndoCommandPort;
    class MailboxSession;
    class MessageListSession;
    class MessageNavigationPort;
    class OnboardingPort;
    class SearchSession;
    struct OpenEmailRoute;
} // namespace javelin::app
namespace javelin::app::undo
{
    struct HistoryFailure;
} // namespace javelin::app::undo
namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
} // namespace javelin::jmap::contacts
namespace javelin::jmap::cache
{
    class AccountReader;
    class IdentityReader;
    class MailboxReader;
    class MailTagReader;
    class MessageViewReader;
} // namespace javelin::jmap::cache

namespace javelin::gui::developer
{
    class DeveloperOptionsDialog;
}

namespace javelin::gui::mailboxes
{
    class MailboxTreeModel;
}

namespace javelin::gui::settings
{
    class GuiSettings;
    struct ConnectionSettings;
} // namespace javelin::gui::settings
namespace javelin::gui::translation
{
    class TranslationService;
}

namespace javelin::gui::shell
{
    class AccountRefreshController;
    class AuthenticationPromptCoordinator;
    class CalendarTabController;
    class ContactsTabController;
    class ComposeTabController;
    class ElidingLabel;
    class MessageCommandController;
    class MessageContentController;
    class MessageFileController;
    class MessageListTabBindingPresenter;
    class MessageListTabPresenter;
    class MessageNavigationController;
    class MessageSelectionController;
    class QuickFilterController;
    class TabBarPresenter;
    class ThemeController;
    struct PersistedMailboxTab;
    struct PersistedSearchTab;
    struct PersistedComposeTab;
    struct PersistedContactsTab;
} // namespace javelin::gui::shell

namespace javelin::gui::messages
{
    enum class MessageListEmptyAction;
    class MessageListPanePresenter;
    class MessageListModel;
} // namespace javelin::gui::messages

namespace javelin::gui::messageview
{
    class MessageViewContainer;
}

namespace javelin::gui::shell
{
    class LayeredStatusBar;
    class MailActionController;
    class MailExportController;
    class MailWorkspaceController;

    using CalendarTabControllerFactory =
        std::function<CalendarTabController*(QStackedWidget&, std::vector<TabState>&, QObject*)>;
    using ContactsTabControllerFactory =
        std::function<ContactsTabController*(QStackedWidget&, std::vector<TabState>&, QObject*)>;
    using ComposeTabControllerFactory =
        std::function<ComposeTabController*(QStackedWidget&, std::vector<TabState>&, QObject*)>;

    struct MainWindowFeatureFactories
    {
        CalendarTabControllerFactory calendar;
        ContactsTabControllerFactory contacts;
        ComposeTabControllerFactory compose;
    };

    class MainWindow : public KXmlGuiWindow
    {
        Q_OBJECT

      public:
        explicit MainWindow(javelin::gui::settings::GuiSettings& settings,
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
                            MainWindowFeatureFactories featureFactories, QWidget* parent = nullptr);
        ~MainWindow() override;
        void openPreferencesForConnection(const QString& connectionId);
        void openMailtoUri(const QString& uri);
        void openInbox();
        void openContacts();
        void openCalendar();
        void composeNewMessage();
        void restoreDraft(const QString& accountId, const QString& draftEmailId,
                          const QString& composeSessionId);
        void openCalendarEvent(const QString& calendarAccountId, const QString& eventId,
                               const QString& recurrenceId, const QDate& navigationDate);

      protected Q_SLOTS:
        void saveNewToolbarConfig() override;

      private:
        struct ClosedTabState;

        static constexpr std::size_t messageWindowSize = 100;

        enum class ToolbarContext
        {
            Mail,
            Compose,
            Contacts,
            Calendar,
        };

        void createActions();
        void routeUndo();
        void routeRedo();
        void routeSaveCurrent();
        void saveSelectedMessages();
        void exportCurrentMailbox();
        void exportCurrentAccount();
        void updateUndoRedoActions();
        void presentHistoryFailure(const javelin::app::undo::HistoryFailure& failure);
        void presentError(const javelin::jmap::OperationError& error);
        void presentUserInterventionError(const QString& message);
        void setupUi();
        void connectSelection();
        void handleCurrentMessageChanged(const QModelIndex& current);
        void openSieveEditor();
        void openSendingIdentities();
        void openSendingIdentitiesFor(QString accountId, QString identityId);
        void composeReply();
        void composeReplyAll();
        void composeForward();
        void editSelectedDraft();
        void activateMailboxSelection(bool refreshRemote);
        void activateMailboxInHomeTab(std::string accountId, std::string mailboxId, QString title,
                                      std::optional<std::string> role, bool refreshRemote);
        void openMailboxSelectionInTab(bool refreshRemote);
        void openComposeForRequest(javelin::jmap::submission::OpenComposeRequest request);
        [[nodiscard]] bool closeComposeTab(int index);
        void
        markTabsStaleForAccount(std::string_view accountId,
                                std::optional<std::string_view> refreshedMailboxId = std::nullopt);
        void markSearchTabsStaleForAccount(std::string_view accountId);
        void executeSearch(const QString& text);
        void showAdvancedSearch();
        void clearSearch();
        void showSortMenu();
        void setEmailListSort(javelin::jmap::query::EmailListSort sort);
        void updateTabBar();
        void activateRelativeTab(int offset);
        void moveMessageSelection(int direction, bool unreadOnly);
        void reopenLastClosedTab();
        void activateTab(int index, bool refreshRemote = false);
        void openOrActivateMailboxTab(std::string accountId, std::string mailboxId, QString title,
                                      std::optional<std::string> role, bool refreshRemote);
        void openOrActivateSearchTab(std::string accountId, QString query, bool refreshRemote);
        void openOrActivateSearchTab(std::string accountId,
                                     javelin::jmap::search::EmailSearchCriteria criteria,
                                     bool refreshRemote);
        void closeTab(int index);
        void loadActiveTabFromCache(bool forceReload = false, bool refreshRemote = true);
        void refreshActiveTabFromServer();
        void refreshTabFromServer(std::size_t tabIndex);
        [[nodiscard]] bool activeTabIsMailbox() const;
        [[nodiscard]] bool activeTabIsSearch() const;
        [[nodiscard]] bool activeTabIsCompose() const;
        [[nodiscard]] bool activeTabIsContacts() const;
        [[nodiscard]] std::optional<std::string> activeAccountId() const;
        [[nodiscard]] std::optional<std::string> preferredMailAccountId() const;
        [[nodiscard]] std::optional<std::string> preferredSubmissionAccountId() const;
        [[nodiscard]] std::optional<std::string> activeMailboxId() const;
        [[nodiscard]] const TabState* activeTab() const;
        [[nodiscard]] TabState* activeTab();
        void applyActiveTabItemsPreservingSelection(std::optional<int> previousMessageRow);
        void maybeLoadMoreMessages();
        void loadMoreMessages();
        void refreshViewsFromCache();
        void refreshFromServer();
        void refreshAccountFromServer(std::string accountId);
        [[nodiscard]] ToolbarContext toolbarContextForActiveTab() const;
        void updateActiveContextUi();
        void openEmailRoute(const javelin::app::OpenEmailRoute& route);
        void resolveOpenEmailRoute();
        void findConversationsWithSender(const QModelIndex& index);
        void showMailboxContextMenu(const QPoint& position);
        void viewSelectedMessageSource();
        void openDeveloperOptions();
        void openPreferences();
        void configureEmailContextMenu();
        void configureCalendarEventContextMenu();
        void reloadAccounts();
        void refreshMessageListPreservingSelection();
        void refreshSelectionFromModels();
        void setMessageViewSelection(std::optional<std::string> accountId,
                                     std::optional<std::string> mailboxId,
                                     std::optional<std::string> emailId);
        void selectPendingInitialMailbox();
        void restorePersistentState();
        void restoreMailboxTab(const PersistedMailboxTab& tab);
        void restoreSearchTab(PersistedSearchTab tab);
        void restoreComposeTab(const PersistedComposeTab& tab);
        void restoreContactsTab(const PersistedContactsTab& tab);
        void savePersistentState() const;
        void updateEmptyStates();
        void activateMessageListEmptyAction();
        void editActiveSearch();
        void updateMessageListHeader();
        void updateSortButton();
        void closeEvent(QCloseEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

        javelin::gui::settings::GuiSettings& m_settings;
        javelin::app::AccountCommandPort& m_accountCommandPort;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        javelin::jmap::cache::MailTagReader& m_mailTagReader;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::cache::IdentityReader& m_identityReader;
        javelin::jmap::cache::MessageViewReader& m_messageViewReader;
        QString m_databasePath;
        javelin::gui::translation::TranslationService& m_translationService;
        javelin::app::DeveloperDiagnosticsPort& m_developerDiagnosticsPort;
        javelin::app::DeveloperMaintenancePort& m_developerMaintenancePort;
        javelin::app::DaemonLogPort& m_daemonLogPort;
        javelin::app::MailCommandPort& m_mailCommandPort;
        javelin::app::SieveCommandPort& m_sieveCommandPort;
        javelin::app::IdentityCommandPort& m_identityCommandPort;
        javelin::app::AccountRefreshPort& m_accountRefreshPort;
        javelin::app::OnboardingPort& m_onboardingPort;
        javelin::app::MessageContentPort& m_messageContentPort;
        javelin::app::MailApplicationEventsPort& m_mailEvents;
        std::unordered_map<std::string, javelin::app::MailAccountStatus> m_accountStatuses;
        javelin::app::MessageNavigationPort& m_messageNavigationPort;
        javelin::app::UndoCommandPort& m_undoCommandPort;
        std::unique_ptr<MailWorkspaceController> m_mailWorkspaceController;
        AccountRefreshController* m_accountRefreshController = nullptr;
        AuthenticationPromptCoordinator* m_authenticationPromptCoordinator = nullptr;
        CalendarTabController* m_calendarTabController = nullptr;
        ContactsTabController* m_contactsTabController = nullptr;
        ComposeTabController* m_composeTabController = nullptr;
        MailActionController* m_mailActionController = nullptr;
        MessageCommandController* m_messageCommandController = nullptr;
        MessageContentController* m_messageContentController = nullptr;
        MessageFileController* m_messageFileController = nullptr;
        MailExportController* m_mailExportController = nullptr;
        std::unique_ptr<MessageListTabBindingPresenter> m_messageListTabBindingPresenter;
        std::unique_ptr<MessageNavigationController> m_messageNavigationController;
        std::unique_ptr<MessageSelectionController> m_messageSelectionController;
        QuickFilterController* m_quickFilterController = nullptr;
        TabBarPresenter* m_tabBarPresenter = nullptr;
        ThemeController* m_themeController = nullptr;
        std::unique_ptr<javelin::gui::messages::MessageListPanePresenter>
            m_messageListPanePresenter;
        std::unique_ptr<MessageListTabPresenter> m_messageListTabPresenter;
        QSplitter* m_mainSplitter = nullptr;
        QStackedWidget* m_contentStack = nullptr;
        QWidget* m_mailboxPane = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxModel = nullptr;
        javelin::gui::messages::MessageListModel* m_messageModel = nullptr;
        javelin::gui::messageview::MessageViewContainer* m_messageViewContainer = nullptr;
        QTabBar* m_tabBar = nullptr;
        QLineEdit* m_mailboxSearchEdit = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        ElidingLabel* m_messageListTitleLabel = nullptr;
        QLabel* m_messageListMetaLabel = nullptr;
        QProgressBar* m_messageLoadingIndicator = nullptr;
        QToolButton* m_searchServerButton = nullptr;
        QToolButton* m_messageSortButton = nullptr;
        QWidget* m_messageListFooter = nullptr;
        QLabel* m_messageListFooterLabel = nullptr;
        QToolButton* m_messageListFooterRetryButton = nullptr;
        QWidget* m_messageEmptyStatePanel = nullptr;
        QLabel* m_messageEmptyState = nullptr;
        QToolButton* m_messageEmptyStateActionButton = nullptr;
        javelin::gui::messages::MessageListEmptyAction m_messageEmptyStateAction{};
        LayeredStatusBar* m_statusBar = nullptr;
        QAction* m_undoAction = nullptr;
        QAction* m_redoAction = nullptr;
        QAction* m_closeTabAction = nullptr;
        QAction* m_previousTabAction = nullptr;
        QAction* m_nextTabAction = nullptr;
        QAction* m_reopenClosedTabAction = nullptr;
        QAction* m_previousMessageAction = nullptr;
        QAction* m_nextMessageAction = nullptr;
        QAction* m_previousUnreadMessageAction = nullptr;
        QAction* m_nextUnreadMessageAction = nullptr;
        QAction* m_focusMailboxTreeAction = nullptr;
        QAction* m_focusMessageListAction = nullptr;
        QAction* m_focusMessageReaderAction = nullptr;
        QAction* m_focusSearchAction = nullptr;
        QAction* m_findInMessageAction = nullptr;
        QAction* m_printMessageAction = nullptr;
        QAction* m_zoomMessageInAction = nullptr;
        QAction* m_zoomMessageOutAction = nullptr;
        QAction* m_resetMessageZoomAction = nullptr;
        QAction* m_refreshAction = nullptr;
        QAction* m_saveCurrentAction = nullptr;
        QAction* m_exportMailboxAction = nullptr;
        QAction* m_exportAccountAction = nullptr;
        QAction* m_quitAction = nullptr;
        QAction* m_preferencesAction = nullptr;
        QAction* m_configureEmailContextMenuAction = nullptr;
        QAction* m_configureCalendarEventContextMenuAction = nullptr;
        QAction* m_developerOptionsAction = nullptr;
        QAction* m_newMessageAction = nullptr;
        QAction* m_contactsAction = nullptr;
        QAction* m_calendarAction = nullptr;
        QAction* m_sieveAction = nullptr;
        QAction* m_sendingIdentitiesAction = nullptr;
        QAction* m_replyAction = nullptr;
        QAction* m_replyAllAction = nullptr;
        QAction* m_forwardAction = nullptr;
        QAction* m_editDraftAction = nullptr;
        QAction* m_archiveAction = nullptr;
        QAction* m_markUnreadAction = nullptr;
        QAction* m_starAction = nullptr;
        QAction* m_junkAction = nullptr;
        QAction* m_tagsAction = nullptr;
        QMenu* m_tagsMenu = nullptr;
        QMenu* m_moveMenu = nullptr;
        QMenu* m_copyMenu = nullptr;
        QAction* m_deleteAction = nullptr;
        QAction* m_permanentDeleteAction = nullptr;
        QAction* m_moveAction = nullptr;
        QAction* m_copyAction = nullptr;
        QAction* m_viewSourceAction = nullptr;
        QAction* m_findSenderContextAction = nullptr;
        QPointer<QMenu> m_emailContextMenu;
        QAction* m_calendarEventEditAction = nullptr;
        QAction* m_calendarEventDuplicateAction = nullptr;
        QAction* m_calendarEventMoveAction = nullptr;
        QAction* m_calendarEventAcceptAction = nullptr;
        QAction* m_calendarEventTentativeAction = nullptr;
        QAction* m_calendarEventDeclineAction = nullptr;
        QAction* m_calendarEventCopyDetailsAction = nullptr;
        QAction* m_calendarEventDeleteAction = nullptr;
        QAction* m_advancedSearchAction = nullptr;
        QAction* m_composeSendAction = nullptr;
        QAction* m_composeScheduleSendAction = nullptr;
        QAction* m_composeAttachFilesAction = nullptr;
        QAction* m_composeSignatureAction = nullptr;
        QAction* m_composeRichTextAction = nullptr;
        QAction* m_contactNewAction = nullptr;
        QAction* m_contactEditAction = nullptr;
        QAction* m_contactDeleteAction = nullptr;
        QAction* m_contactCopyAction = nullptr;
        QAction* m_contactImportAction = nullptr;
        QAction* m_contactExportAction = nullptr;
        QAction* m_contactDuplicatesAction = nullptr;
        QAction* m_contactAddToGroupAction = nullptr;
        QMenu* m_contactAddToGroupMenu = nullptr;
        QAction* m_contactRemoveFromGroupAction = nullptr;
        QMenu* m_contactRemoveFromGroupMenu = nullptr;
        QAction* m_contactManageAddressBooksAction = nullptr;
        QMenu* m_contactAddressBooksMenu = nullptr;
        QAction* m_contactRefreshAction = nullptr;
        QAction* m_calendarNewEventAction = nullptr;
        QAction* m_calendarPreviousMonthAction = nullptr;
        QAction* m_calendarTodayAction = nullptr;
        QAction* m_calendarNextMonthAction = nullptr;
        QAction* m_calendarListAction = nullptr;
        QAction* m_calendarRefreshAction = nullptr;
        javelin::jmap::query::EmailListSort& m_emailListSort;
        std::optional<int>& m_activeTabIndex;
        std::vector<TabState>& m_tabs;
        bool m_modelUpdateInProgress = false;
        std::unique_ptr<ClosedTabState> m_lastClosedTab;
        javelin::gui::developer::DeveloperOptionsDialog* m_developerOptionsDialog = nullptr;
        std::optional<std::string> m_pendingInitialMailboxAccountId;
    };

} // namespace javelin::gui::shell
