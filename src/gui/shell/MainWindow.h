#pragma once

#include <QMainWindow>

class QComboBox;
class QCloseEvent;
class QLabel;
class QListView;
class QSplitter;
class QTreeView;
class QAction;

namespace javelin::jmap
{
    class JmapCore;
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

    class MainWindow : public QMainWindow
    {
      public:
        explicit MainWindow(javelin::jmap::JmapCore& jmapCore,
                            javelin::jmap::cache::AccountRepository& accountRepository,
                            javelin::jmap::cache::MessageViewService& messageViewService,
                            javelin::jmap::cache::QueryService& queryService,
                            QWidget* parent = nullptr);
        ~MainWindow() override = default;

      private:
        void createMenus();
        void setupUi();
        void connectSelection();
        void refreshViewsFromCache();
        void refreshFromServer();
        void refreshSelectedMailboxMessages(std::string accountId, std::string mailboxId);
        void refreshSelectedMessageContent(std::string accountId, std::string emailId);
        void openPreferences();
        void reloadAccounts();
        void restoreSelection(std::optional<std::string> mailboxId,
                              std::optional<std::string> emailId);
        void restorePersistentState();
        void savePersistentState() const;
        void updateEmptyStates();
        void closeEvent(QCloseEvent* event) override;

        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::MessageViewService& m_messageViewService;
        javelin::jmap::cache::QueryService& m_queryService;
        QComboBox* m_accountCombo = nullptr;
        QSplitter* m_mainSplitter = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxModel = nullptr;
        javelin::gui::messages::MessageListModel* m_messageModel = nullptr;
        javelin::gui::messageview::MessageViewContainer* m_messageViewContainer = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        QLabel* m_messageEmptyState = nullptr;
        QAction* m_refreshAction = nullptr;
        QAction* m_preferencesAction = nullptr;
        bool m_refreshInFlight = false;
    };

} // namespace javelin::gui::shell
