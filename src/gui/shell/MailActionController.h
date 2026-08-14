#pragma once

#include "gui/shell/TabWorkspace.h"

#include <QList>
#include <QModelIndex>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>

#include <functional>

class QAction;
class QListView;
class QMenu;
class QWidget;

namespace javelin::app
{
    class MailCommandPort;
}
namespace javelin::jmap
{
    struct OperationError;
}
namespace javelin::jmap::cache
{
    class MailboxReader;
    class MailTagReader;
} // namespace javelin::jmap::cache

namespace javelin::gui::shell
{
    class MessageCommandController;
    class MessageSelectionController;
    class QuickFilterController;

    struct MailActions
    {
        QAction& newMessage;
        QAction& reply;
        QAction& replyAll;
        QAction& forward;
        QAction& editDraft;
        QAction& archive;
        QAction& markUnread;
        QAction& star;
        QAction& junk;
        QAction& tags;
        QAction& deleteFromMailbox;
        QAction& permanentDelete;
        QAction& move;
        QAction& copy;
        QAction& viewSource;
    };

    class MailActionController final : public QObject
    {
        Q_OBJECT

      public:
        MailActionController(javelin::jmap::cache::MailboxReader& mailboxReader,
                             javelin::jmap::cache::MailTagReader& mailTagReader,
                             javelin::app::MailCommandPort& mailCommandPort,
                             MessageSelectionController& selectionController,
                             MessageCommandController& commandController,
                             QuickFilterController& quickFilterController, QListView& messageView,
                             QMenu& tagsMenu, QWidget& parentWidget, MailActions actions,
                             std::function<void(QString, int)> showStatus,
                             std::function<void(const javelin::jmap::OperationError&)> showError,
                             std::function<void()> refreshMessageList, QObject* parent = nullptr);

        void activate(const TabState* tab);
        void update();
        void rebuildTagsMenu();
        void configureContextMenu(QMenu& menu,
                                  std::function<std::vector<QString>()> configuredLayout,
                                  std::function<void(const QList<QAction*>&)> replaceActionList);
        void showContextMenu(const QPoint& position,
                             std::function<void(QModelIndex)> findConversationsWithSender);

      private:
        [[nodiscard]] bool selectedMessagesAreStarred() const;
        [[nodiscard]] bool selectedMessagesAreJunk() const;
        void createTag(bool applyToSelection);
        void showTagManager();
        [[nodiscard]] std::optional<std::string> activeAccountId() const;
        [[nodiscard]] std::optional<std::string> activeMailboxId() const;
        [[nodiscard]] bool activeTabIsSearch() const;

        javelin::jmap::cache::MailboxReader& m_mailboxReader;
        javelin::jmap::cache::MailTagReader& m_mailTagReader;
        javelin::app::MailCommandPort& m_mailCommandPort;
        MessageSelectionController& m_selectionController;
        MessageCommandController& m_commandController;
        QuickFilterController& m_quickFilterController;
        QListView& m_messageView;
        QMenu& m_tagsMenu;
        QWidget& m_parentWidget;
        MailActions m_actions;
        std::function<void(QString, int)> m_showStatus;
        std::function<void(const javelin::jmap::OperationError&)> m_showError;
        std::function<void()> m_refreshMessageList;
        QPointer<QMenu> m_contextMenu;
        std::function<std::vector<QString>()> m_configuredContextMenuLayout;
        std::function<void(const QList<QAction*>&)> m_replaceContextMenuActionList;
        QList<QObject*> m_contextMenuObjects;
        const TabState* m_activeTab = nullptr;
    };
} // namespace javelin::gui::shell
