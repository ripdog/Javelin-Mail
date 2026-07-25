#pragma once

#include "gui/messages/MessageSelectionRestoration.h"
#include "gui/shell/TabWorkspace.h"
#include "jmap/cache/QueryService.h"

#include <QString>

#include <optional>
#include <string>
#include <vector>

class QListView;
class QTreeView;

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
    class MessageSelectionController
    {
      public:
        MessageSelectionController(javelin::gui::mailboxes::MailboxTreeModel& mailboxModel,
                                   QTreeView& mailboxView,
                                   javelin::gui::messages::MessageListModel& messageModel,
                                   QListView& messageView);

        [[nodiscard]] std::optional<std::string> currentEmailId() const;
        [[nodiscard]] std::optional<std::string> currentThreadId() const;
        [[nodiscard]] std::optional<int> currentRow() const;
        [[nodiscard]] std::vector<javelin::gui::messages::MessageRowIdentity> rowIdentities() const;
        [[nodiscard]] std::vector<std::string> selectedEmailIds() const;
        [[nodiscard]] std::vector<javelin::jmap::cache::MessageListItem>
        selectedMessageSummaries() const;

        void syncTabSelection(TabState* tab) const;
        void restoreSelection(std::optional<std::string> accountId,
                              std::optional<std::string> mailboxId,
                              std::optional<std::string> threadId,
                              std::optional<std::string> emailId,
                              bool scrollToSelection = true) const;
        [[nodiscard]] bool restoreTabSelection(const TabState* tab,
                                               std::optional<int> previousMessageRow) const;
        [[nodiscard]] bool selectMessageAlone(const QString& emailId) const;

      private:
        javelin::gui::mailboxes::MailboxTreeModel& m_mailboxModel;
        QTreeView& m_mailboxView;
        javelin::gui::messages::MessageListModel& m_messageModel;
        QListView& m_messageView;
    };
} // namespace javelin::gui::shell
