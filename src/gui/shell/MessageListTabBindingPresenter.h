#pragma once

#include "gui/shell/TabWorkspace.h"

namespace javelin::gui::mailboxes
{
    class MailboxTreeModel;
}

namespace javelin::gui::messages
{
    class MessageListModel;
}

class QLineEdit;
class QTreeView;
class QWidget;

namespace javelin::gui::shell
{
    class MessageListTabBindingPresenter
    {
      public:
        MessageListTabBindingPresenter(javelin::gui::mailboxes::MailboxTreeModel& mailboxModel,
                                       QTreeView& mailboxView, QLineEdit& searchEdit,
                                       javelin::gui::messages::MessageListModel& messageModel,
                                       QWidget& mailboxPane);

        void syncNavigation(const TabState* tab, bool showMailboxPane) const;
        void applyPage(const TabState* tab) const;

      private:
        javelin::gui::mailboxes::MailboxTreeModel& m_mailboxModel;
        QTreeView& m_mailboxView;
        QLineEdit& m_searchEdit;
        javelin::gui::messages::MessageListModel& m_messageModel;
        QWidget& m_mailboxPane;
    };
} // namespace javelin::gui::shell
