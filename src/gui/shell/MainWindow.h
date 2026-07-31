#pragma once

#include "app/MailApplicationService.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/cache/QueryService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/submission/ComposeTypes.h"

#include <KXmlGuiWindow>
#include <QModelIndex>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QListView;
class QPoint;
class QSplitter;
class QStackedWidget;
class QSpinBox;
class QTabBar;
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
    class ComposeService;
    class ContactCommandPort;
    class MailboxSession;
    class MessageListSession;
    class MessageNavigationCoordinator;
    class SearchSession;
    class TranslationService;
    struct OpenEmailRoute;
} // namespace javelin::app
namespace javelin::app::undo
{
    class UndoManager;
    struct HistoryFailure;
} // namespace javelin::app::undo
namespace javelin::jmap::contacts
{
    class ContactIdentityLookup;
} // namespace javelin::jmap::contacts
namespace javelin::jmap::calendar
{
    class CalendarService;
}

namespace javelin::jmap::cache
{
    class AccountReader;
    class AccountRepository;
    class ContactRepository;
    class IdentityRepository;
    class MessageViewService;
    class QueryService;
} // namespace javelin::jmap::cache

namespace javelin::gui::mailboxes
{
    class MailboxTreeModel;
}

namespace javelin::gui::settings
{
    struct ConnectionSettings;
}

namespace javelin::gui::shell
{
    class AccountRefreshController;
    class CalendarTabController;
    class ContactsTabController;
    class ComposeTabController;
    class ElidingLabel;
    class MessageCommandController;
    class MessageContentController;
    class MessageFileController;
    class MessageListTabBindingPresenter;
    class MessageListTabController;
    class MessageListTabPresenter;
    class MessageNavigationController;
    class MessageSelectionController;
    class TabBarPresenter;
    struct PersistedMailboxTab;
    struct PersistedSearchTab;
    struct PersistedComposeTab;
    struct PersistedContactsTab;
} // namespace javelin::gui::shell

namespace javelin::gui::messages
{
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

    class MainWindow : public KXmlGuiWindow
    {
        Q_OBJECT

      public:
        explicit MainWindow(
            javelin::jmap::cache::AccountRepository& accountRepository,
            javelin::jmap::cache::AccountReader& accountReader,
            javelin::jmap::cache::ContactRepository& contactRepository,
            javelin::jmap::calendar::CalendarService& calendarService,
            javelin::jmap::contacts::ContactIdentityLookup& contactIdentityLookup,
            javelin::jmap::cache::IdentityRepository& identityRepository,
            javelin::jmap::cache::MessageViewService& messageViewService,
            javelin::jmap::cache::QueryService& queryService,
            javelin::app::TranslationService& translationService,
            javelin::app::ComposeService& composeService,
            javelin::app::ContactCommandPort& contactCommandPort,
            javelin::app::MailApplicationService& mailService,
            javelin::app::MessageNavigationCoordinator& messageNavigationCoordinator,
            javelin::app::undo::UndoManager& undoManager, QWidget* parent = nullptr);
        ~MainWindow() override;
        void openPreferencesForConnection(const QString& connectionId);
        void restoreDraft(const QString& accountId, const QString& draftEmailId,
                          const QString& composeSessionId);

      Q_SIGNALS:
        void accountSettingsChanged();

      protected Q_SLOTS:
        void saveNewToolbarConfig() override;

      private:
        static constexpr std::size_t pageSize = 100;

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
        void updateUndoRedoActions();
        void presentHistoryFailure(const javelin::app::undo::HistoryFailure& failure);
        void presentError(const javelin::jmap::OperationError& error);
        void presentUserInterventionError(const QString& message);
        void setupUi();
        void connectSelection();
        void handleCurrentMessageChanged(const QModelIndex& current);
        void composeNewMessage();
        void openContacts();
        void openCalendar();
        void openSieveEditor();
        void composeReply();
        void composeReplyAll();
        void composeForward();
        void editSelectedDraft();
        void activateMailboxSelection(bool refreshRemote);
        void activateMailboxInHomeTab(std::string accountId, std::string mailboxId, QString title,
                                      std::optional<std::string> role,
                                      std::optional<std::size_t> total, bool refreshRemote);
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
        [[nodiscard]] bool activeTabIsCalendar() const;
        [[nodiscard]] std::optional<std::string> activeAccountId() const;
        [[nodiscard]] std::optional<std::string> activeMailboxId() const;
        [[nodiscard]] const TabState* activeTab() const;
        [[nodiscard]] TabState* activeTab();
        void applyActiveTabPagePreservingSelection(std::optional<int> previousMessageRow);
        void goToFirstPage();
        void goToLastPage();
        void goToPage(std::size_t pageIndex);
        void goToPreviousPage();
        void goToNextPage();
        void refreshViewsFromCache();
        void refreshFromServer();
        void refreshAccountFromServer(std::string accountId);
        [[nodiscard]] ToolbarContext toolbarContextForActiveTab() const;
        void updateToolbarForActiveTab();
        void openEmailRoute(const javelin::app::OpenEmailRoute& route);
        void resolveOpenEmailRoute();
        void findConversationsWithSender(const QModelIndex& index);
        void showMailboxContextMenu(const QPoint& position);
        void showMessageListContextMenu(const QPoint& position);
        void viewSelectedMessageSource();
        void openPreferences();
        void reloadAccounts();
        void refreshMessageListPreservingSelection();
        void refreshSelectionFromModels();
        void restorePersistentState();
        void restoreMailboxTab(const PersistedMailboxTab& tab);
        void restoreSearchTab(PersistedSearchTab tab);
        void restoreComposeTab(const PersistedComposeTab& tab);
        void restoreContactsTab(const PersistedContactsTab& tab);
        void savePersistentState() const;
        void updateEmptyStates();
        void updateMessageListHeader();
        void updateMessageActions();
        void updateSortButton();
        void scheduleApplicationPaletteRefresh();
        void applyApplicationPalette();
        void updatePaletteDependentIcons();
        void changeEvent(QEvent* event) override;
        void closeEvent(QCloseEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::AccountReader& m_accountReader;
        javelin::jmap::cache::ContactRepository& m_contactRepository;
        javelin::jmap::calendar::CalendarService& m_calendarService;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::cache::IdentityRepository& m_identityRepository;
        javelin::jmap::cache::MessageViewService& m_messageViewService;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::app::TranslationService& m_translationService;
        javelin::app::ComposeService& m_composeService;
        javelin::app::ContactCommandPort& m_contactCommandPort;
        javelin::app::MailApplicationService& m_mailService;
        javelin::app::MessageNavigationCoordinator& m_messageNavigationCoordinator;
        javelin::app::undo::UndoManager& m_undoManager;
        AccountRefreshController* m_accountRefreshController = nullptr;
        CalendarTabController* m_calendarTabController = nullptr;
        ContactsTabController* m_contactsTabController = nullptr;
        ComposeTabController* m_composeTabController = nullptr;
        MessageCommandController* m_messageCommandController = nullptr;
        MessageContentController* m_messageContentController = nullptr;
        MessageFileController* m_messageFileController = nullptr;
        std::unique_ptr<MessageListTabBindingPresenter> m_messageListTabBindingPresenter;
        MessageListTabController* m_messageListTabController = nullptr;
        std::unique_ptr<MessageNavigationController> m_messageNavigationController;
        std::unique_ptr<MessageSelectionController> m_messageSelectionController;
        TabBarPresenter* m_tabBarPresenter = nullptr;
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
        QLabel* m_messagePageLabel = nullptr;
        QToolButton* m_searchServerButton = nullptr;
        QToolButton* m_messageSortButton = nullptr;
        QToolButton* m_firstPageButton = nullptr;
        QToolButton* m_previousPageButton = nullptr;
        QSpinBox* m_pageNumberSpinBox = nullptr;
        QToolButton* m_nextPageButton = nullptr;
        QToolButton* m_lastPageButton = nullptr;
        QLabel* m_messageEmptyState = nullptr;
        LayeredStatusBar* m_statusBar = nullptr;
        QAction* m_undoAction = nullptr;
        QAction* m_redoAction = nullptr;
        QAction* m_refreshAction = nullptr;
        QAction* m_quitAction = nullptr;
        QAction* m_preferencesAction = nullptr;
        QAction* m_newMessageAction = nullptr;
        QAction* m_contactsAction = nullptr;
        QAction* m_calendarAction = nullptr;
        QAction* m_sieveAction = nullptr;
        QAction* m_replyAction = nullptr;
        QAction* m_replyAllAction = nullptr;
        QAction* m_forwardAction = nullptr;
        QAction* m_editDraftAction = nullptr;
        QAction* m_archiveAction = nullptr;
        QAction* m_markUnreadAction = nullptr;
        QAction* m_deleteAction = nullptr;
        QAction* m_permanentDeleteAction = nullptr;
        QAction* m_moveAction = nullptr;
        QAction* m_copyAction = nullptr;
        QAction* m_viewSourceAction = nullptr;
        QAction* m_advancedSearchAction = nullptr;
        QAction* m_composeSendAction = nullptr;
        QAction* m_composeSaveDraftAction = nullptr;
        QAction* m_composeAttachFilesAction = nullptr;
        QAction* m_contactNewAction = nullptr;
        QAction* m_contactEditAction = nullptr;
        QAction* m_contactDeleteAction = nullptr;
        QAction* m_contactCopyAction = nullptr;
        QAction* m_contactImportAction = nullptr;
        QAction* m_contactExportAction = nullptr;
        QAction* m_contactDuplicatesAction = nullptr;
        QAction* m_contactAddToGroupAction = nullptr;
        QAction* m_contactRemoveFromGroupAction = nullptr;
        QAction* m_contactManageAddressBooksAction = nullptr;
        QAction* m_contactRefreshAction = nullptr;
        QAction* m_calendarNewEventAction = nullptr;
        QAction* m_calendarPreviousMonthAction = nullptr;
        QAction* m_calendarTodayAction = nullptr;
        QAction* m_calendarNextMonthAction = nullptr;
        QAction* m_calendarListAction = nullptr;
        QAction* m_calendarRefreshAction = nullptr;
        javelin::jmap::query::EmailListSort m_emailListSort;
        bool m_paletteRefreshPending = false;
        std::optional<int> m_activeTabIndex;
        std::vector<TabState> m_tabs;
    };

} // namespace javelin::gui::shell
