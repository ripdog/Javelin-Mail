#pragma once

#include "app/LongPollCoordinator.h"
#include "jmap/cache/QueryService.h"
#include "jmap/query/EmailListSort.h"
#include "jmap/search/EmailSearch.h"
#include "jmap/submission/ComposeTypes.h"

#include <KXmlGuiWindow>
#include <QIcon>
#include <QModelIndex>
#include <QTemporaryDir>

#include <cstdint>
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
class QPoint;
class QSplitter;
class QStackedWidget;
class QSettings;
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
}
namespace javelin::gui::search
{
    class SearchSession;
}
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
    class TranslationCacheRepository;
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
}

namespace javelin::gui::messages
{
    class MessageListModel;
}

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
            javelin::jmap::cache::TranslationCacheRepository& translationCacheRepository,
            javelin::app::ComposeService& composeService,
            javelin::app::MailApplicationService& mailService, QWidget* parent = nullptr);
        ~MainWindow() override = default;
        void openMessageFromNotification(const QString& accountId, const QString& mailboxId,
                                         const QString& threadId, const QString& emailId);
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

        struct PageState
        {
            std::size_t offset = 0;
            std::size_t position = 0;
            std::size_t returnedLimit = pageSize;
            std::optional<std::size_t> total;
            std::string queryState;
            std::optional<std::string> anchor;
            std::vector<javelin::jmap::cache::MessageListItem> items;
            bool cacheLoaded = false;
            bool refreshInFlight = false;
            bool stale = false;
            QString refreshError;
        };

        struct MailboxTabState
        {
            std::string accountId;
            std::string mailboxId;
            QString title;
            std::optional<std::string> role;
            PageState page;
            TabSelectionState selection;
            std::optional<javelin::app::MailboxObservation> observationId;
        };

        struct SearchTabState
        {
            javelin::gui::search::SearchSession* session = nullptr;
            TabSelectionState selection;
        };

        struct ComposeTabState
        {
            std::string accountId;
            std::string composeSessionId;
            QString title;
            javelin::gui::compose::ComposeTabWidget* widget = nullptr;
            PageState page;
            TabSelectionState selection;
        };

        struct ContactsTabState
        {
            std::string accountId;
            QString title;
            javelin::gui::contacts::ContactsManagerWidget* widget = nullptr;
            PageState page;
            TabSelectionState selection;
        };

        struct CalendarTabState
        {
            std::string accountId;
            QString title;
            javelin::gui::calendar::MonthCalendarWidget* widget = nullptr;
            PageState page;
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

        struct MessageContentRequestState
        {
            std::string accountId;
            std::string emailId;
            std::uint64_t token = 0;
        };

        void createActions();
        void presentError(const javelin::jmap::OperationError& error,
                          const QString& title = QStringLiteral("Action Required"));
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
        void loadMailboxTabFromCache(std::string_view accountId, std::string_view mailboxId,
                                     bool applyIfActive,
                                     std::optional<std::size_t> requiredOffset = std::nullopt);
        [[nodiscard]] bool loadMailboxTabPageFromCache(MailboxTabState& tab,
                                                       bool forceReload = false);
        void ensureMailboxObservation(MailboxTabState& tab);
        void releaseMailboxObservation(MailboxTabState& tab);
        void refreshActiveTabFromServer();
        void refreshTabFromServer(std::size_t tabIndex);
        void refreshMailboxTabFromServer(MailboxTabState& tab);
        void connectSearchSession(javelin::gui::search::SearchSession& session);
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
        void goToPreviousPage();
        void goToNextPage();
        void refreshViewsFromCache();
        void refreshFromServer();
        void refreshAccountFromServer(std::string accountId);
        void refreshConnectionSettings(javelin::gui::settings::ConnectionSettings settings);
        void updateWindowTitle();
        [[nodiscard]] ToolbarContext toolbarContextForActiveTab() const;
        void updateToolbarForActiveTab();
        void refreshSelectedMessageContent(std::string accountId, std::string emailId);
        [[nodiscard]] std::vector<std::string> selectedEmailIds() const;
        [[nodiscard]] std::variant<std::vector<std::string>, QString>
        selectedEmailIdsForMailboxAction(std::string_view accountId) const;
        [[nodiscard]] std::vector<javelin::jmap::cache::MessageListItem>
        selectedMessageSummaries() const;
        void selectMessageAlone(const QString& emailId);
        void queueDeleteEmail(std::string accountId, std::string mailboxId, std::string emailId);
        void queueArchiveEmails(std::string accountId, std::optional<std::string> sourceMailboxId,
                                std::vector<std::string> emailIds);
        void queueDeleteEmails(std::string accountId, std::string mailboxId,
                               std::vector<std::string> emailIds);
        void queueDestroyEmails(std::string accountId, std::vector<std::string> emailIds);
        void queueMoveEmails(std::string accountId, std::optional<std::string> sourceMailboxId,
                             std::string destinationMailboxId, std::vector<std::string> emailIds,
                             QString successMessage);
        void queueCopyEmails(std::string accountId, std::optional<std::string> sourceMailboxId,
                             std::string destinationMailboxId, std::vector<std::string> emailIds,
                             QString successMessage);
        void queueMoveEmail(std::string accountId, std::string sourceMailboxId,
                            std::string destinationMailboxId, std::string emailId,
                            QString successMessage);
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
        void saveAttachment(std::string accountId, std::string emailId, std::string partId);
        void saveAllAttachments(std::string accountId, std::string emailId);
        void openAttachment(std::string accountId, std::string emailId, std::string partId);
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
        void restoreMailboxTab(const QSettings& settings, const QString& accountId);
        void restoreSearchTab(const QSettings& settings, const QString& accountId);
        void restoreComposeTab(const QSettings& settings);
        void restoreContactsTab(const QSettings& settings, const QString& accountId);
        [[nodiscard]] javelin::gui::contacts::ContactsManagerWidget*
        appendContactsTab(std::string ownerAccountId, QString title);
        void savePersistentState() const;
        void writePersistentTab(QSettings& settings, const TabState& tab) const;
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
        javelin::jmap::cache::TranslationCacheRepository& m_translationCacheRepository;
        javelin::app::ComposeService& m_composeService;
        javelin::app::MailApplicationService& m_mailService;
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
        QToolButton* m_messageQuickFilterButton = nullptr;
        QToolButton* m_messageSortButton = nullptr;
        QToolButton* m_previousPageButton = nullptr;
        QToolButton* m_nextPageButton = nullptr;
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
        bool m_syncingNavigation = false;
        std::optional<int> m_activeTabIndex;
        std::vector<TabState> m_tabs;
        QTemporaryDir m_openAttachmentDirectory;
    };

} // namespace javelin::gui::shell
