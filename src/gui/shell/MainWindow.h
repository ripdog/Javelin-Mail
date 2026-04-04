#pragma once

#include <QMainWindow>

class QComboBox;
class QLabel;
class QListView;
class QTreeView;

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::jmap::cache
{
    class AccountRepository;
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

namespace javelin::gui::shell
{

    class MainWindow : public QMainWindow
    {
      public:
        explicit MainWindow(javelin::jmap::JmapCore& jmapCore,
                            javelin::jmap::cache::AccountRepository& accountRepository,
                            javelin::jmap::cache::QueryService& queryService,
                            QWidget* parent = nullptr);
        ~MainWindow() override = default;

      private:
        void setupUi();
        void connectSelection();
        void reloadAccounts();
        void updateEmptyStates();

        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::cache::AccountRepository& m_accountRepository;
        javelin::jmap::cache::QueryService& m_queryService;
        QComboBox* m_accountCombo = nullptr;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxModel = nullptr;
        javelin::gui::messages::MessageListModel* m_messageModel = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        QLabel* m_messageEmptyState = nullptr;
    };

} // namespace javelin::gui::shell
