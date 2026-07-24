#pragma once

#include "app/MailApplicationService.h"
#include "jmap/cache/QueryService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/submission/ComposeTypes.h"

#include <KXmlGuiWindow>
#include <QIcon>
#include <QModelIndex>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QListView;
class QMenu;
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
    class MailboxSession;
    class MessageListSession;
    class MessageNavigationCoordinator;
    class SearchSession;
    class TranslationService;
    struct OpenEmailRoute;
} // namespace javelin::app
namespace javelin::jmap::contacts
{
    class ContactService;
    class ContactIdentityLookup;
} // namespace javelin::jmap::contacts
namespace javelin::jmap::calendar
{
    class CalendarService;
}

namespace javelin::jmap::cache
{
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
    class ElidingLabel;
    class MessageFileController;
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

namespace javelin::gui::compose
{
    class ComposeTabWidget;
}
namespace javelin::gui::contacts
{
    class ContactsManagerWidget;
}
namespace javelin::gui::calendar
{
    class MonthCalendarWidget;
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
            QWidget* parent = nullptr);
        ~MainWindow() override;
        void openPreferencesForConnection(const QString& connectionId);

      Q_SIGNALS:
        void accountSettingsChanged();

      protected Q_SLOTS:
        void saveNewToolbarConfig() override;

      private:
        static constexpr std::size_t pageSize = 100;

        struct TabSelectionState
        {
            std::optional<std::string> threadId;
            std::optional<std::string> emailId;
            std::vector<std::string> selectedEmailIds;
        };

        struct MailboxTabState
        {
            javelin::app::MailboxSession* session = nullptr;
            TabSelectionState selection;
        };

        struct SearchTabState
        {
            javelin::app::SearchSession* session = nullptr;
            TabSelectionState selection;
        };

        struct ComposeTabState
        {
            std::string accountId;
            std::string composeSessionId;
            QString title;
            javelin::gui::compose::ComposeTabWidget* widget = nullptr;
            TabSelectionState selection;
        };

        struct ContactsTabState
        {
            std::string accountId;
            QString title;
            javelin::gui::contacts::ContactsManagerWidget* widget = nullptr;
            TabSelectionState selection;
        };

        struct CalendarTabState
        {
            std::string accountId;
            QString title;
            javelin::gui::calendar::MonthCalendarWidget* widget = nullptr;
            TabSelectionState selection;
        };

        struct TabState
        {
            std::variant<MailboxTabState, SearchTabState, ComposeTabState, ContactsTabState,
                         CalendarTabState>
                content;
        };

        enum class ToolbarContext
        {
            Mail,
            Compose,
            Contacts,
            Calendar,
        };

        enum class MessageTransferOperation
        {
            Move,
            Copy,
        };

        struct MessageContentRequestState
        {
            std::string accountId;
            std::string emailId;
            std::uint64_t token = 0;
        };

        void createActions();
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
        void openOrActivateComposeTab(javelin::jmap::submission::DraftSnapshot snapshot);
        void attachComposeWidget(javelin::gui::compose::ComposeTabWidget* widget, int tabIndex);
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
        void syncNavigationForActiveTab();
        void syncActiveTabSelectionFromViews();
        void loadActiveTabFromCache(bool forceReload = false, bool refreshRemote = true);
        void refreshActiveTabFromServer();
        void refreshTabFromServer(std::size_t tabIndex);
        void connectMessageListSession(javelin::app::MessageListSession& session);
        void connectSearchSession(javelin::app::SearchSession& session);
        [[nodiscard]] QString titleForTab(const TabState& tab) const;
        [[nodiscard]] QString mailboxTitle(const MailboxTabState& tab) const;
        [[nodiscard]] QIcon iconForTab(const TabState& tab) const;
        [[nodiscard]] bool activeTabIsMailbox() const;
        [[nodiscard]] bool activeTabIsSearch() const;
        [[nodiscard]] bool activeTabIsCompose() const;
        [[nodiscard]] bool activeTabIsContacts() const;
        [[nodiscard]] bool activeTabIsCalendar() const;
        [[nodiscard]] std::optional<std::string> activeAccountId() const;
        [[nodiscard]] std::optional<std::string> activeMailboxId() const;
        [[nodiscard]] const TabState* activeTab() const;
        [[nodiscard]] TabState* activeTab();
        void applyActiveTabPageToModel();
        void applyActiveTabPagePreservingSelection(std::optional<int> previousMessageRow);
        void goToFirstPage();
        void goToLastPage();
        void goToPage(std::size_t pageIndex);
        void goToPreviousPage();
        void goToNextPage();
        void refreshViewsFromCache();
        void refreshActiveSearchAfterMutation(std::string_view accountId);
        void refreshFromServer();
        void refreshAccountFromServer(std::string accountId);
        void refreshConnectionSettings(javelin::gui::settings::ConnectionSettings settings);
        void updateWindowTitle();
        [[nodiscard]] ToolbarContext toolbarContextForActiveTab() const;
        void updateToolbarForActiveTab();
        void refreshSelectedMessageContent(std::string accountId, std::string emailId);
        void openEmailRoute(const javelin::app::OpenEmailRoute& route);
        void resolveOpenEmailRoute();
        [[nodiscard]] const javelin::app::OpenEmailRoute* activeOpenEmailRoute() const;
        [[nodiscard]] std::vector<std::string> selectedEmailIds() const;
        [[nodiscard]] javelin::app::MessageSelection
        selectedMessageActionItems(bool excludeUnread = false) const;
        [[nodiscard]] std::vector<javelin::jmap::cache::MessageListItem>
        selectedMessageSummaries() const;
        void selectMessageAlone(const QString& emailId);
        void queueArchiveEmails(std::string accountId, std::optional<std::string> sourceMailboxId,
                                javelin::app::MessageSelection selection);
        void queueDeleteEmails(std::string accountId, std::string mailboxId,
                               javelin::app::MessageSelection selection);
        void queueDestroyEmails(std::string accountId, std::optional<std::string> sourceMailboxId,
                                javelin::app::MessageSelection selection);
        void populateMailboxDestinationMenus(QMenu* moveMenu, QMenu* copyMenu,
                                             std::string accountId,
                                             std::optional<std::string> sourceMailboxId,
                                             javelin::app::MessageSelection selection);
        void queueTransferEmails(std::string accountId, std::optional<std::string> sourceMailboxId,
                                 std::string destinationMailboxId,
                                 javelin::app::MessageSelection selection,
                                 MessageTransferOperation operation, QString successMessage);
        void queueMarkEmailRead(std::string accountId, std::string emailId);
        void toggleMessageFlagged(const QModelIndex& index);
        void markSelectedEmailUnread();
        void archiveSelectedEmail();
        void deleteSelectedEmail();
        void permanentlyDeleteSelectedEmail();
        void showMoveMenu();
        void showCopyMenu();
        void findConversationsWithSender(const QModelIndex& index);
        void showMailboxContextMenu(const QPoint& position);
        void showMessageListContextMenu(const QPoint& position);
        void viewSelectedMessageSource();
        void openPreferences();
        void reloadAccounts();
        void restoreSelection(std::optional<std::string> accountId,
                              std::optional<std::string> mailboxId,
                              std::optional<std::string> threadId,
                              std::optional<std::string> emailId, bool scrollToSelection = true);
        [[nodiscard]] bool restoreActiveTabMessageSelection(std::optional<int> previousMessageRow);
        [[nodiscard]] bool restoreSelectionAfterMessageRefresh(
            std::optional<std::string> accountId, std::optional<std::string> mailboxId,
            std::optional<std::string> threadId, std::optional<std::string> emailId,
            const std::vector<std::string>& selectedEmailIds,
            std::optional<int> previousMessageRow);
        [[nodiscard]] QModelIndex restoreMessageSelection(std::optional<std::string> threadId,
                                                          std::optional<std::string> emailId);
        void refreshMessageListPreservingSelection();
        void submitQueuedEmailMutations(std::string accountId);
        void refreshSelectionFromModels();
        void restorePersistentState();
        void restoreMailboxTab(const PersistedMailboxTab& tab);
        void restoreSearchTab(PersistedSearchTab tab);
        void restoreComposeTab(const PersistedComposeTab& tab);
        void restoreContactsTab(const PersistedContactsTab& tab);
        [[nodiscard]] javelin::gui::contacts::ContactsManagerWidget*
        appendContactsTab(std::string ownerAccountId, QString title);
        void savePersistentState() const;
        void updateEmptyStates();
        void updateMessageListHeader();
        void updateMessageActions();
        void updateSortButton();
        void closeEvent(QCloseEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::ContactRepository& m_contactRepository;
        javelin::jmap::contacts::ContactService& m_contactService;
        javelin::jmap::calendar::CalendarService& m_calendarService;
        javelin::jmap::contacts::ContactIdentityLookup& m_contactIdentityLookup;
        javelin::jmap::cache::IdentityRepository& m_identityRepository;
        javelin::jmap::cache::MessageViewService& m_messageViewService;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::app::TranslationService& m_translationService;
        javelin::app::ComposeService& m_composeService;
        javelin::app::MailApplicationService& m_mailService;
        javelin::app::MessageNavigationCoordinator& m_messageNavigationCoordinator;
        MessageFileController* m_messageFileController = nullptr;
        std::unique_ptr<javelin::gui::messages::MessageListPanePresenter>
            m_messageListPanePresenter;
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
        bool m_refreshInFlight = false;
        std::uint64_t m_nextMessageContentRequestToken = 1;
        std::optional<MessageContentRequestState> m_messageContentRequestInFlight;
        std::optional<std::uint64_t> m_navigationContextRequested;
        bool m_syncingNavigation = false;
        std::optional<int> m_activeTabIndex;
        std::vector<TabState> m_tabs;
    };

} // namespace javelin::gui::shell
