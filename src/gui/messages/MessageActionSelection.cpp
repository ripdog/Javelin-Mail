#include "gui/messages/MessageActionSelection.h"

#include "gui/messages/MessageListModel.h"

#include <algorithm>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <vector>

namespace javelin::gui::messages
{
    namespace
    {
        [[nodiscard]] bool indexIsUnread(const QModelIndex& index)
        {
            return index.isValid() && index.data(MessageListModel::IsUnreadRole).toBool();
        }
    } // namespace

    javelin::app::MessageSelection messageSelectionForAction(QModelIndexList selectedRows,
                                                             const QModelIndex& currentIndex,
                                                             const bool excludeUnread)
    {
        std::vector<QModelIndex> indexes;
        indexes.reserve(static_cast<std::size_t>(selectedRows.size() + 1));
        for (const auto& index : selectedRows)
        {
            if (index.isValid())
            {
                indexes.push_back(index);
            }
        }
        if (indexes.empty() && currentIndex.isValid())
        {
            indexes.push_back(currentIndex);
        }

        std::ranges::sort(indexes, [](const QModelIndex& left, const QModelIndex& right)
                          { return left.row() < right.row(); });

        javelin::app::MessageSelection selection;
        std::unordered_set<std::string> seenEmailIds;
        std::unordered_set<std::string> seenThreadIds;
        for (const auto& index : indexes)
        {
            if (excludeUnread && indexIsUnread(index))
            {
                continue;
            }

            auto emailId = index.data(MessageListModel::EmailIdRole).toString().toStdString();
            auto threadId = index.data(MessageListModel::ThreadIdRole).toString().toStdString();
            const auto rowKind = static_cast<MessageListModel::RowKind>(
                index.data(MessageListModel::RowKindRole).toInt());
            const bool collapsedThreadSummary =
                rowKind == MessageListModel::RowKind::ThreadSummary &&
                !index.data(MessageListModel::IsExpandedRole).toBool();
            if (collapsedThreadSummary)
            {
                if (!threadId.empty() && seenThreadIds.insert(threadId).second)
                {
                    selection.emplace_back(javelin::app::SelectedCollapsedThread{
                        .threadId = std::move(threadId),
                    });
                }
                continue;
            }

            if (!emailId.empty() && seenEmailIds.insert(emailId).second)
            {
                selection.emplace_back(javelin::app::SelectedEmail{.emailId = std::move(emailId)});
            }
        }
        return selection;
    }

} // namespace javelin::gui::messages
