#pragma once

#include "jmap/cache/QueryService.h"

#include <KXmlGuiWindow>
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
class QTabBar;
class QToolButton;
class QTreeView;
class QAction;
class QWidget;

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::app
{
    class LongPollService;
}

namespace javelin::jmap::cache
{
    class AccountRepository;
    class MessageViewService;
    class QueryService;
} // namespace javelin::jmap::cache

namespace javelin::gui::mailboxes
{
    class MailboxTreeModel;
}

namespace javelin::gui::messages
{
    class MessageListModel;
}

namespace javelin::gui::messageview
{
    class MessageViewContainer;
}

namespace javelin::gui::shell
{

    class MainWindow : public KXmlGuiWindow
    {
        Q_OBJECT

      public:
        explicit MainWindow(javelin::jmap::JmapCore& jmapCore,
                            javelin::jmap::cache::AccountRepository& accountRepository,
                            javelin::jmap::cache::MessageViewService& messageViewService,
                            javelin::jmap::cache::QueryService& queryService,
                            javelin::app::LongPollService& longPollService,
                            QWidget* parent = nullptr);
        ~MainWindow() override = default;
        void openMessageFromNotification(const QString& accountId, const QString& mailboxId,
                                         const QString& threadId, const QString& emailId);

      private:
        static constexpr std::size_t pageSize = 100;

        struct TabSelectionState
        {
            std::optional<std::string> threadId;
            std::optional<std::string> emailId;
        };

        struct PageState
        {
            std::size_t offset = 0;
            std::optional<std::size_t> total;
            std::vector<javelin::jmap::cache::MessageListItem> items;
            bool cacheLoaded = false;
            bool refreshInFlight = false;
        };

        struct MailboxTabState
        {
            std::string accountId;
            std::string mailboxId;
            QString title;
            PageState page;
            TabSelectionState selection;
        };

        struct SearchTabState
        {
            std::string accountId;
            std::string query;
            QString title;
            PageState page;
            TabSelectionState selection;
        };

        struct TabState
        {
            std::variant<MailboxTabState, SearchTabState> content;
        };

        void createActions();
        void setupUi();
        void connectSelection();
        void activateMailboxSelection(bool refreshRemote);
        void activateMailboxInHomeTab(std::string accountId, std::string mailboxId, QString title,
                                      std::optional<std::size_t> total, bool refreshRemote);
        void openMailboxSelectionInTab(bool refreshRemote);
        void executeSearch(const QString& text);
        void clearSearch();
        void updateTabBar();
        void activateTab(int index, bool refreshRemote = false);
        void openOrActivateMailboxTab(std::string accountId, std::string mailboxId, QString title,
                                      bool refreshRemote);
        void openOrActivateSearchTab(std::string accountId, QString query, bool refreshRemote);
        void closeTab(int index);
        void syncNavigationForActiveTab();
        void syncActiveTabSelectionFromViews();
        void loadActiveTabFromCache(bool forceReload = false);
        void loadMailboxTabPageFromCache(MailboxTabState& tab, bool forceReload = false);
        void applySearchTabCachedPage(SearchTabState& tab, bool forceReload = false);
        void refreshActiveTabFromServer();
        void refreshTabFromServer(std::size_t tabIndex);
        void refreshMailboxTabFromServer(MailboxTabState& tab);
        void refreshSearchTabFromServer(SearchTabState& tab);
        [[nodiscard]] bool shouldRefreshMailboxTabFromServer(const MailboxTabState& tab) const;
        [[nodiscard]] QString titleForTab(const TabState& tab) const;
        [[nodiscard]] bool activeTabIsMailbox() const;
        [[nodiscard]] bool activeTabIsSearch() const;
        [[nodiscard]] std::optional<std::string> activeAccountId() const;
        [[nodiscard]] std::optional<std::string> activeMailboxId() const;
        [[nodiscard]] const TabState* activeTab() const;
        [[nodiscard]] TabState* activeTab();
        void applyActiveTabPageToModel();
        void goToPreviousPage();
        void goToNextPage();
        void refreshViewsFromCache();
        void refreshFromServer();
        void refreshSelectedMessageContent(std::string accountId, std::string emailId);
        void queueArchiveEmail(std::string accountId, std::string mailboxId, std::string emailId);
        void queueDeleteEmail(std::string accountId, std::string mailboxId, std::string emailId);
        void queueMoveEmail(std::string accountId, std::string sourceMailboxId,
                            std::string destinationMailboxId, std::string emailId,
                            QString successMessage);
        void queueMarkEmailRead(std::string accountId, std::string emailId);
        void markSelectedEmailUnread();
        void archiveSelectedEmail();
        void deleteSelectedEmail();
        void showMoveMenu();
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
                              std::optional<std::string> emailId);
        void restoreSelectionAfterMessageRefresh(std::optional<std::string> accountId,
                                                 std::optional<std::string> mailboxId,
                                                 std::optional<std::string> threadId,
                                                 std::optional<std::string> emailId,
                                                 std::optional<int> previousMessageRow);
        [[nodiscard]] QModelIndex restoreMessageSelection(std::optional<std::string> threadId,
                                                          std::optional<std::string> emailId);
        void refreshMessageListPreservingSelection();
        void submitQueuedEmailMutations(std::string accountId);
        void refreshSelectionFromModels();
        void restorePersistentState();
        void savePersistentState() const;
        void updateLongPollStatus();
        void updateEmptyStates();
        void updateMessageListHeader();
        void updateMessageActions();
        void closeEvent(QCloseEvent* event) override;
        bool eventFilter(QObject* watched, QEvent* event) override;

        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::MessageViewService& m_messageViewService;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::app::LongPollService& m_longPollService;
        QSplitter* m_mainSplitter = nullptr;
        QWidget* m_mailboxPane = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxModel = nullptr;
        javelin::gui::messages::MessageListModel* m_messageModel = nullptr;
        javelin::gui::messageview::MessageViewContainer* m_messageViewContainer = nullptr;
        QTabBar* m_tabBar = nullptr;
        QLineEdit* m_mailboxSearchEdit = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        QLabel* m_messageListTitleLabel = nullptr;
        QLabel* m_messageListMetaLabel = nullptr;
        QLabel* m_longPollStatusLabel = nullptr;
        QLabel* m_messagePageLabel = nullptr;
        QToolButton* m_messageQuickFilterButton = nullptr;
        QToolButton* m_previousPageButton = nullptr;
        QToolButton* m_nextPageButton = nullptr;
        QLabel* m_messageEmptyState = nullptr;
        QAction* m_refreshAction = nullptr;
        QAction* m_quitAction = nullptr;
        QAction* m_preferencesAction = nullptr;
        QAction* m_archiveAction = nullptr;
        QAction* m_markUnreadAction = nullptr;
        QAction* m_deleteAction = nullptr;
        QAction* m_moveAction = nullptr;
        QAction* m_viewSourceAction = nullptr;
        bool m_refreshInFlight = false;
        bool m_syncingNavigation = false;
        std::optional<int> m_activeTabIndex;
        std::vector<TabState> m_tabs;
        QTemporaryDir m_openAttachmentDirectory;
    };

} // namespace javelin::gui::shell
