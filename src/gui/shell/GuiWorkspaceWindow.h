#pragma once

#include "app/GuiCacheReader.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QModelIndex>

#include <memory>

class QListView;
class QLabel;
class QStandardItemModel;
class QTextBrowser;
class QTreeView;

namespace javelin::app
{
    class GuiDaemonSession;
}

namespace javelin::gui::shell
{

    class GuiWorkspaceWindow final : public QMainWindow
    {
        Q_OBJECT

      public:
        explicit GuiWorkspaceWindow(javelin::app::GuiDaemonSession& session,
                                    QWidget* parent = nullptr);
        ~GuiWorkspaceWindow() override;

        void refresh();

      private:
        void populateCache(javelin::app::GuiCacheReader::SnapshotResult result);
        void loadMessages(const QString& accountId, const QString& mailboxId);
        void populateMessages(javelin::app::GuiCacheReader::MessageListResult result);
        void loadMessage(const QString& accountId, const QString& emailId);
        void populateMessage(javelin::app::GuiCacheReader::MessageDetailResult result);
        void requestRemoteRefresh();
        void showAccountSettings();
        void showSelectedMessage(const QModelIndex& index);
        void showError(const QString& message);

        javelin::app::GuiDaemonSession& m_session;
        javelin::app::GuiCacheReader m_cacheReader;
        QTreeView* m_mailboxView = nullptr;
        QListView* m_messageView = nullptr;
        QTextBrowser* m_messageBody = nullptr;
        QLabel* m_statusLabel = nullptr;
        QStandardItemModel* m_mailboxModel = nullptr;
        QStandardItemModel* m_messageModel = nullptr;
        QFutureWatcher<javelin::app::GuiCacheReader::SnapshotResult> m_snapshotWatcher;
        QFutureWatcher<javelin::app::GuiCacheReader::MessageListResult> m_messageWatcher;
        QFutureWatcher<javelin::app::GuiCacheReader::MessageDetailResult> m_detailWatcher;
        QString m_selectedAccountId;
        QString m_selectedMailboxId;
        QString m_selectedEmailId;
        bool m_refreshPending = false;
    };

} // namespace javelin::gui::shell
