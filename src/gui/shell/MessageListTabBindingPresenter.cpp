#include "gui/shell/MessageListTabBindingPresenter.h"

#include "app/MailboxSession.h"
#include "app/SearchSession.h"
#include "gui/mailboxes/MailboxSelection.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListModel.h"

#include <QItemSelectionModel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QTreeView>
#include <QWidget>

namespace javelin::gui::shell
{
    void restoreRepresentedThreadExpansions(javelin::gui::messages::MessageListModel& messageModel,
                                            std::vector<std::string>& expandedThreadIds)
    {
        std::erase_if(expandedThreadIds,
                      [&messageModel](const std::string& threadId)
                      {
                          return !messageModel.isThreadExpanded(threadId) &&
                                 !messageModel.setThreadExpanded(threadId, true);
                      });
    }

    MessageListTabBindingPresenter::MessageListTabBindingPresenter(
        javelin::gui::mailboxes::MailboxTreeModel& mailboxModel, QTreeView& mailboxView,
        QLineEdit& searchEdit, javelin::gui::messages::MessageListModel& messageModel,
        QWidget& mailboxPane)
        : m_mailboxModel(mailboxModel), m_mailboxView(mailboxView), m_searchEdit(searchEdit),
          m_messageModel(messageModel), m_mailboxPane(mailboxPane)
    {
    }

    void MessageListTabBindingPresenter::syncNavigation(const TabState* tab,
                                                        const bool showMailboxPane) const
    {
        m_mailboxPane.setVisible(showMailboxPane);
        if (tab == nullptr)
            return;

        QSignalBlocker mailboxBlocker{m_mailboxView.selectionModel()};
        QSignalBlocker searchBlocker{&m_searchEdit};
        if (auto* mailbox = std::get_if<MailboxTabState>(&tab->content);
            mailbox != nullptr && mailbox->session != nullptr)
        {
            const auto index = javelin::gui::mailboxes::findMailboxIndexForSelection(
                m_mailboxModel, QString::fromStdString(mailbox->session->accountId()),
                std::optional<QString>{QString::fromStdString(mailbox->session->mailboxId())});
            if (index.isValid())
            {
                m_mailboxView.setCurrentIndex(index);
                m_mailboxView.scrollTo(index);
            }
            m_searchEdit.clear();
            return;
        }

        if (auto* search = std::get_if<SearchTabState>(&tab->content);
            search != nullptr && search->session != nullptr)
        {
            const auto index = javelin::gui::mailboxes::findMailboxIndexForSelection(
                m_mailboxModel, QString::fromStdString(search->session->accountId()), std::nullopt);
            if (index.isValid())
            {
                m_mailboxView.setCurrentIndex(index);
                m_mailboxView.scrollTo(index);
            }
            m_searchEdit.setText(QString::fromStdString(search->session->query()));
            return;
        }

        m_searchEdit.clear();
    }

    void MessageListTabBindingPresenter::applyItems(TabState* tab) const
    {
        if (tab == nullptr)
        {
            m_messageModel.clear();
            m_appliedSession = nullptr;
            m_appliedItemsRevision = 0;
            return;
        }

        if (auto* mailbox = std::get_if<MailboxTabState>(&tab->content);
            mailbox != nullptr && mailbox->session != nullptr)
        {
            const auto& state = mailbox->session->state();
            if (m_appliedSession == mailbox->session &&
                m_appliedItemsRevision == state.itemsRevision &&
                m_messageModel.isBoundTo(mailbox->session->accountId(),
                                         mailbox->session->mailboxId()))
            {
                return;
            }
            m_messageModel.setItems(mailbox->session->accountId(), mailbox->session->mailboxId(),
                                    state.items);
            if (auto* expandedThreadIds = tabExpandedThreadIds(*tab))
                restoreRepresentedThreadExpansions(m_messageModel, *expandedThreadIds);
            m_appliedSession = mailbox->session;
            m_appliedItemsRevision = state.itemsRevision;
            return;
        }

        if (auto* search = std::get_if<SearchTabState>(&tab->content);
            search != nullptr && search->session != nullptr)
        {
            const auto& state = search->session->state();
            if (m_appliedSession == search->session &&
                m_appliedItemsRevision == state.itemsRevision &&
                m_messageModel.isBoundTo(search->session->accountId(), std::nullopt))
            {
                return;
            }
            m_messageModel.setItems(search->session->accountId(), std::nullopt, state.items);
            if (auto* expandedThreadIds = tabExpandedThreadIds(*tab))
                restoreRepresentedThreadExpansions(m_messageModel, *expandedThreadIds);
            m_appliedSession = search->session;
            m_appliedItemsRevision = state.itemsRevision;
            return;
        }

        m_messageModel.clear();
        m_appliedSession = nullptr;
        m_appliedItemsRevision = 0;
    }
} // namespace javelin::gui::shell
