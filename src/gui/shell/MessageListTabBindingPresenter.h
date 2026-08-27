#pragma once

#include "gui/shell/TabWorkspace.h"

#include <cstdint>
#include <string>
#include <vector>

namespace javelin::app
{
    class MessageListSession;
}

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
    void restoreRepresentedThreadExpansions(javelin::gui::messages::MessageListModel& messageModel,
                                            std::vector<std::string>& expandedThreadIds);

    class MessageListTabBindingPresenter
    {
      public:
        MessageListTabBindingPresenter(javelin::gui::mailboxes::MailboxTreeModel& mailboxModel,
                                       QTreeView& mailboxView, QLineEdit& searchEdit,
                                       javelin::gui::messages::MessageListModel& messageModel,
                                       QWidget& mailboxPane);

        void syncNavigation(const TabState* tab, bool showMailboxPane) const;
        void applyItems(TabState* tab) const;

      private:
        javelin::gui::mailboxes::MailboxTreeModel& m_mailboxModel;
        QTreeView& m_mailboxView;
        QLineEdit& m_searchEdit;
        javelin::gui::messages::MessageListModel& m_messageModel;
        QWidget& m_mailboxPane;
        mutable const javelin::app::MessageListSession* m_appliedSession = nullptr;
        mutable std::uint64_t m_appliedItemsRevision = 0;
    };
} // namespace javelin::gui::shell
