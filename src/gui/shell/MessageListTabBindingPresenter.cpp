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
        if (const auto* mailbox = std::get_if<MailboxTabState>(&tab->content);
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

        if (const auto* search = std::get_if<SearchTabState>(&tab->content);
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

    void MessageListTabBindingPresenter::applyPage(const TabState* tab) const
    {
        if (tab == nullptr)
        {
            m_messageModel.clear();
            return;
        }

        if (const auto* mailbox = std::get_if<MailboxTabState>(&tab->content);
            mailbox != nullptr && mailbox->session != nullptr)
        {
            m_messageModel.setPage(mailbox->session->accountId(), mailbox->session->mailboxId(),
                                   mailbox->session->page().items);
            return;
        }

        if (const auto* search = std::get_if<SearchTabState>(&tab->content);
            search != nullptr && search->session != nullptr)
        {
            m_messageModel.setPage(search->session->accountId(), std::nullopt,
                                   search->session->page().items);
            return;
        }

        m_messageModel.clear();
    }
} // namespace javelin::gui::shell
