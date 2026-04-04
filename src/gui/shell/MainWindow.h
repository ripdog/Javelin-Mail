#pragma once

#include <QMainWindow>

class QLabel;
class QListView;
class QTreeView;

namespace javelin::jmap
{
    class JmapCore;
}

namespace javelin::jmap::cache
{
    class QueryService;
}

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
                            javelin::jmap::cache::QueryService& queryService,
                            QWidget* parent = nullptr);
        ~MainWindow() override = default;

      private:
        void setupUi();
        void connectSelection();
        void updateEmptyStates();

        javelin::jmap::JmapCore& m_jmapCore;
        javelin::jmap::cache::QueryService& m_queryService;
        javelin::gui::mailboxes::MailboxTreeModel* m_mailboxModel = nullptr;
        javelin::gui::messages::MessageListModel* m_messageModel = nullptr;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        QLabel* m_messageEmptyState = nullptr;
    };

} // namespace javelin::gui::shell
