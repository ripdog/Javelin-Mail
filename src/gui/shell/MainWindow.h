#pragma once

#include <KXmlGuiWindow>
#include <QModelIndex>
#include <QTemporaryDir>

class QCloseEvent;
class QLabel;
class QListView;
class QPoint;
class QSplitter;
class QToolButton;
class QTreeView;
class QAction;

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
        void createActions();
        void setupUi();
        void connectSelection();
        void refreshViewsFromCache();
        void refreshFromServer();
        void refreshSelectedMailboxMessages(std::string accountId, std::string mailboxId);
        void refreshSelectedMessageContent(std::string accountId, std::string emailId);
        void queueArchiveEmail(std::string accountId, std::string mailboxId, std::string emailId);
        void queueDeleteEmail(std::string accountId, std::string mailboxId, std::string emailId);
        void queueMoveEmail(std::string accountId, std::string sourceMailboxId,
                            std::string destinationMailboxId, std::string emailId,
                            QString successMessage);
        void archiveSelectedEmail();
        void deleteSelectedEmail();
        void showMoveMenu();
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
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxModel = nullptr;
        javelin::gui::messages::MessageListModel* m_messageModel = nullptr;
        javelin::gui::messageview::MessageViewContainer* m_messageViewContainer = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        QLabel* m_messageListTitleLabel = nullptr;
        QLabel* m_messageListMetaLabel = nullptr;
        QLabel* m_longPollStatusLabel = nullptr;
        QToolButton* m_messageQuickFilterButton = nullptr;
        QLabel* m_messageEmptyState = nullptr;
        QAction* m_refreshAction = nullptr;
        QAction* m_quitAction = nullptr;
        QAction* m_preferencesAction = nullptr;
        QAction* m_archiveAction = nullptr;
        QAction* m_deleteAction = nullptr;
        QAction* m_moveAction = nullptr;
        QAction* m_viewSourceAction = nullptr;
        bool m_refreshInFlight = false;
        QTemporaryDir m_openAttachmentDirectory;
    };

} // namespace javelin::gui::shell
