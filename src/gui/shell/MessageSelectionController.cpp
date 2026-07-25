#include "gui/shell/MessageSelectionController.h"

#include "gui/mailboxes/MailboxSelection.h"
#include "gui/mailboxes/MailboxTreeModel.h"
#include "gui/messages/MessageListModel.h"

#include <QItemSelection>
#include <QItemSelectionModel>
#include <QListView>
#include <QModelIndexList>
#include <QTreeView>

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace javelin::gui::shell
{
    MessageSelectionController::MessageSelectionController(
        javelin::gui::mailboxes::MailboxTreeModel& mailboxModel, QTreeView& mailboxView,
        javelin::gui::messages::MessageListModel& messageModel, QListView& messageView)
        : m_mailboxModel(mailboxModel), m_mailboxView(mailboxView), m_messageModel(messageModel),
          m_messageView(messageView)
    {
    }

    std::optional<std::string> MessageSelectionController::currentEmailId() const
    {
        const auto index = m_messageView.currentIndex();
        if (!index.isValid())
            return std::nullopt;

        const auto emailId =
            index.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString();
        return emailId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                 : std::optional<std::string>{emailId.toStdString()};
    }

    std::optional<std::string> MessageSelectionController::currentThreadId() const
    {
        const auto index = m_messageView.currentIndex();
        if (!index.isValid())
            return std::nullopt;

        const auto threadId =
            index.data(javelin::gui::messages::MessageListModel::ThreadIdRole).toString();
        return threadId.isEmpty() ? std::optional<std::string>{std::nullopt}
                                  : std::optional<std::string>{threadId.toStdString()};
    }

    std::optional<int> MessageSelectionController::currentRow() const
    {
        const auto index = m_messageView.currentIndex();
        return index.isValid() ? std::optional<int>{index.row()} : std::nullopt;
    }

    std::vector<javelin::gui::messages::MessageRowIdentity>
    MessageSelectionController::rowIdentities() const
    {
        std::vector<javelin::gui::messages::MessageRowIdentity> rows;
        rows.reserve(static_cast<std::size_t>(m_messageModel.rowCount()));
        for (int row = 0; row < m_messageModel.rowCount(); ++row)
        {
            const auto index = m_messageModel.index(row, 0);
            rows.push_back({
                .threadId = index.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                                .toString()
                                .toStdString(),
                .emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                               .toString()
                               .toStdString(),
            });
        }
        return rows;
    }

    std::vector<std::string> MessageSelectionController::selectedEmailIds() const
    {
        std::vector<std::string> emailIds;
        std::unordered_set<std::string> seen;
        const auto* selectionModel = m_messageView.selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList indexes = selectionModel->selectedRows();
            emailIds.reserve(static_cast<std::size_t>(indexes.size()));
            for (const auto& index : indexes)
            {
                auto emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                                   .toString()
                                   .toStdString();
                if (!emailId.empty() && seen.insert(emailId).second)
                    emailIds.push_back(std::move(emailId));
            }
        }

        if (emailIds.empty())
        {
            if (const auto emailId = currentEmailId(); emailId.has_value())
                emailIds.push_back(*emailId);
        }
        return emailIds;
    }

    std::vector<javelin::jmap::cache::MessageListItem>
    MessageSelectionController::selectedMessageSummaries() const
    {
        std::vector<QModelIndex> indexes;
        const auto* selectionModel = m_messageView.selectionModel();
        if (selectionModel != nullptr)
        {
            const QModelIndexList selectedRows = selectionModel->selectedRows();
            indexes.reserve(static_cast<std::size_t>(selectedRows.size()));
            for (const auto& index : selectedRows)
                if (index.isValid())
                    indexes.push_back(index);
        }
        std::ranges::sort(indexes, [](const QModelIndex& left, const QModelIndex& right)
                          { return left.row() < right.row(); });

        std::vector<javelin::jmap::cache::MessageListItem> summaries;
        summaries.reserve(indexes.size());
        std::unordered_set<std::string> seen;
        for (const auto& index : indexes)
        {
            auto emailId = index.data(javelin::gui::messages::MessageListModel::EmailIdRole)
                               .toString()
                               .toStdString();
            if (emailId.empty() || !seen.insert(emailId).second)
                continue;

            const auto subject =
                index.data(javelin::gui::messages::MessageListModel::SubjectRole).toString();
            const auto preview =
                index.data(javelin::gui::messages::MessageListModel::PreviewRole).toString();
            summaries.push_back(javelin::jmap::cache::MessageListItem{
                .emailId = std::move(emailId),
                .threadId = index.data(javelin::gui::messages::MessageListModel::ThreadIdRole)
                                .toString()
                                .toStdString(),
                .subject = subject.isEmpty() ? std::nullopt
                                             : std::optional<std::string>{subject.toStdString()},
                .preview = preview.isEmpty() ? std::nullopt
                                             : std::optional<std::string>{preview.toStdString()},
                .receivedAt = index.data(javelin::gui::messages::MessageListModel::ReceivedAtRole)
                                  .toString()
                                  .toStdString(),
                .sentAt = std::nullopt,
                .threadMessageCount =
                    index.data(javelin::gui::messages::MessageListModel::ThreadMessageCountRole)
                        .toULongLong(),
                .hasAttachment =
                    index.data(javelin::gui::messages::MessageListModel::HasAttachmentRole)
                        .toBool(),
                .isUnread =
                    index.data(javelin::gui::messages::MessageListModel::IsUnreadRole).toBool(),
                .isFlagged =
                    index.data(javelin::gui::messages::MessageListModel::IsFlaggedRole).toBool(),
                .from = std::nullopt,
                .mailboxNames = {},
            });
        }
        return summaries;
    }

    void MessageSelectionController::syncTabSelection(TabState* tab) const
    {
        if (tab == nullptr)
            return;

        tabSelection(*tab) = {
            .threadId = currentThreadId(),
            .emailId = currentEmailId(),
            .selectedEmailIds = selectedEmailIds(),
        };
    }

    void MessageSelectionController::restoreSelection(std::optional<std::string> accountId,
                                                      std::optional<std::string> mailboxId,
                                                      std::optional<std::string> threadId,
                                                      std::optional<std::string> emailId,
                                                      const bool scrollToSelection) const
    {
        if (accountId.has_value())
        {
            const auto mailboxIdValue =
                mailboxId.has_value() ? std::optional<QString>{QString::fromStdString(*mailboxId)}
                                      : std::optional<QString>{std::nullopt};
            const auto mailboxIndex = javelin::gui::mailboxes::findMailboxIndexForSelection(
                m_mailboxModel, QString::fromStdString(*accountId), mailboxIdValue);
            if (mailboxIndex.isValid())
            {
                m_mailboxView.setCurrentIndex(mailboxIndex);
                if (scrollToSelection)
                    m_mailboxView.scrollTo(mailboxIndex);
            }
        }

        const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
            rowIdentities(), {
                                 .threadId = std::move(threadId),
                                 .emailId = std::move(emailId),
                                 .selectedEmailIds = {},
                                 .previousRow = std::nullopt,
                             });
        if (!plan.currentRow.has_value())
            return;

        const auto index = m_messageModel.index(static_cast<int>(*plan.currentRow), 0);
        if (!index.isValid())
            return;

        m_messageView.setCurrentIndex(index);
        if (scrollToSelection)
            m_messageView.scrollTo(index);
    }

    bool MessageSelectionController::restoreTabSelection(
        const TabState* tab, const std::optional<int> previousMessageRow) const
    {
        if (tab == nullptr)
            return false;

        const auto& selection = tabSelection(*tab);
        const auto plan = javelin::gui::messages::planMessageSelectionRestoration(
            rowIdentities(),
            {
                .threadId = selection.threadId,
                .emailId = selection.emailId,
                .selectedEmailIds = selection.selectedEmailIds,
                .previousRow =
                    previousMessageRow.has_value() && *previousMessageRow >= 0
                        ? std::optional<std::size_t>{static_cast<std::size_t>(*previousMessageRow)}
                        : std::nullopt,
            });

        auto* selectionModel = m_messageView.selectionModel();
        if (selectionModel != nullptr && !plan.selectedRows.empty())
        {
            QItemSelection restoredSelection;
            for (const auto row : plan.selectedRows)
            {
                const auto index = m_messageModel.index(static_cast<int>(row), 0);
                if (index.isValid())
                    restoredSelection.select(index, index);
            }
            selectionModel->select(restoredSelection,
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }

        if (!plan.currentRow.has_value())
            return false;

        const auto currentIndex = m_messageModel.index(static_cast<int>(*plan.currentRow), 0);
        if (!currentIndex.isValid())
            return false;

        if (selectionModel != nullptr && !plan.selectedRows.empty())
        {
            selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
        }
        else if (selectionModel != nullptr && (plan.currentEmailChanged || plan.fallbackSelected))
        {
            selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::ClearAndSelect |
                                                              QItemSelectionModel::Rows);
        }
        else
        {
            m_messageView.setCurrentIndex(currentIndex);
        }

        if (plan.fallbackSelected)
            m_messageView.scrollTo(currentIndex);
        return plan.currentEmailChanged || plan.fallbackSelected;
    }

    bool MessageSelectionController::selectMessageAlone(const QString& emailId) const
    {
        if (emailId.isEmpty())
            return false;

        QModelIndex match;
        for (int row = 0; row < m_messageModel.rowCount(); ++row)
        {
            const auto index = m_messageModel.index(row, 0);
            if (index.data(javelin::gui::messages::MessageListModel::EmailIdRole).toString() ==
                emailId)
            {
                match = index;
                break;
            }
        }
        if (!match.isValid())
            return false;

        auto* selectionModel = m_messageView.selectionModel();
        if (selectionModel != nullptr)
            selectionModel->select(match,
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_messageView.setCurrentIndex(match);
        m_messageView.scrollTo(match);
        return true;
    }
} // namespace javelin::gui::shell
