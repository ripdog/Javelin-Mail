#include "gui/messages/MessageListModel.h"

#include <QString>

#include <algorithm>

namespace javelin::gui::messages
{
    namespace
    {

        [[nodiscard]] bool containsThreadId(const std::vector<std::string>& threadIds,
                                            const std::string_view threadId)
        {
            return std::ranges::find(threadIds, threadId) != threadIds.end();
        }

    } // namespace

    MessageListModel::MessageListModel(javelin::jmap::cache::QueryService& queryService,
                                       QObject* parent)
        : QAbstractListModel(parent), m_queryService(queryService)
    {
    }

    MessageListModel::~MessageListModel() = default;

    int MessageListModel::rowCount(const QModelIndex& parent) const
    {
        if (parent.isValid())
        {
            return 0;
        }

        return static_cast<int>(m_rows.size());
    }

    QVariant MessageListModel::data(const QModelIndex& index, const int role) const
    {
        if (!index.isValid() || index.row() < 0 ||
            static_cast<std::size_t>(index.row()) >= m_rows.size())
        {
            return {};
        }

        const auto& row = m_rows[static_cast<std::size_t>(index.row())];
        const auto& item = itemForRow(row);
        const auto sender = item.from.has_value()
                                ? QString::fromStdString(item.from->name.value_or(item.from->email))
                                : QStringLiteral("(unknown sender)");
        const auto subject = item.subject.has_value() ? QString::fromStdString(*item.subject)
                                                      : QStringLiteral("(no subject)");
        const auto preview =
            item.preview.has_value() ? QString::fromStdString(*item.preview) : QString{};

        if (role == Qt::DisplayRole)
        {
            return QStringLiteral("%1  %2").arg(sender, subject);
        }

        if (role == Qt::ToolTipRole)
        {
            return preview;
        }

        if (role == EmailIdRole)
        {
            return QString::fromStdString(item.emailId);
        }

        if (role == ThreadIdRole)
        {
            return QString::fromStdString(item.threadId);
        }

        if (role == SenderDisplayRole)
        {
            return sender;
        }

        if (role == SubjectRole)
        {
            return subject;
        }

        if (role == PreviewRole)
        {
            return preview;
        }

        if (role == ReceivedAtRole)
        {
            return QString::fromStdString(item.receivedAt);
        }

        if (role == HasAttachmentRole)
        {
            return item.hasAttachment;
        }

        if (role == IsUnreadRole)
        {
            return item.isUnread;
        }

        if (role == IsFlaggedRole)
        {
            return item.isFlagged;
        }

        if (role == ThreadMessageCountRole)
        {
            return static_cast<qulonglong>(item.threadMessageCount);
        }

        if (role == RowKindRole)
        {
            return static_cast<int>(row.kind);
        }

        if (role == IsExpandedRole)
        {
            return row.kind == RowKind::ThreadSummary && isThreadExpanded(item.threadId);
        }

        if (role == CanExpandRole)
        {
            return row.kind == RowKind::ThreadSummary && item.threadMessageCount > 1;
        }

        return {};
    }

    void MessageListModel::setMailboxContext(std::optional<std::string> accountId,
                                             std::optional<std::string> mailboxId)
    {
        if (m_accountId == accountId && m_mailboxId == mailboxId && !m_searchQuery.has_value())
        {
            return;
        }

        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        m_searchQuery.reset();
        reload();
    }

    void
    MessageListModel::setSearchResults(std::string accountId, std::string query,
                                       std::vector<javelin::jmap::cache::MessageListItem> results)
    {
        beginResetModel();
        m_accountId = std::move(accountId);
        m_mailboxId.reset();
        m_searchQuery = std::move(query);
        m_threads.clear();
        m_threads.reserve(results.size());
        for (auto& result : results)
        {
            m_threads.push_back(ThreadEntry{
                .summary = std::move(result),
                .members = {},
                .membersLoaded = false,
            });
        }
        rebuildVisibleRows();
        endResetModel();
    }

    void MessageListModel::clearSearch()
    {
        if (!m_searchQuery.has_value())
        {
            return;
        }

        m_searchQuery.reset();
        reload();
    }

    void MessageListModel::refresh()
    {
        if (m_searchQuery.has_value())
        {
            // Search results are an explicit server snapshot. Keep them stable until the user
            // reruns or clears the search instead of collapsing back to an empty mailbox context.
            return;
        }

        reload();
    }

    bool MessageListModel::setThreadExpanded(const std::string_view threadId, const bool expanded)
    {
        const auto threadIndex = findThreadIndex(threadId);
        if (!threadIndex.has_value())
        {
            return false;
        }

        const bool alreadyExpanded = isThreadExpanded(threadId);
        if (alreadyExpanded == expanded)
        {
            return false;
        }

        if (expanded)
        {
            if (!loadThreadMembers(*threadIndex))
            {
                return false;
            }
            const auto summaryRow = visibleSummaryRowForThread(*threadIndex);
            if (!summaryRow.has_value())
            {
                return false;
            }

            const int memberCount = static_cast<int>(m_threads[*threadIndex].members.size());
            if (memberCount <= 0)
            {
                return false;
            }

            m_expandedThreadIds.push_back(std::string{threadId});
            beginInsertRows(QModelIndex{}, *summaryRow + 1, *summaryRow + memberCount);
            for (int memberRow = 0; memberRow < memberCount; ++memberRow)
            {
                m_rows.insert(m_rows.begin() + (*summaryRow + 1 + memberRow),
                              VisibleRow{
                                  .kind = RowKind::ThreadMember,
                                  .threadIndex = *threadIndex,
                                  .memberIndex = static_cast<std::size_t>(memberRow),
                              });
            }
            endInsertRows();
            const QModelIndex summaryIndex = index(*summaryRow, 0);
            Q_EMIT dataChanged(summaryIndex, summaryIndex,
                               {IsExpandedRole, CanExpandRole, ThreadMessageCountRole});
            return true;
        }

        m_expandedThreadIds.erase(
            std::remove(m_expandedThreadIds.begin(), m_expandedThreadIds.end(), threadId),
            m_expandedThreadIds.end());
        const auto summaryRow = visibleSummaryRowForThread(*threadIndex);
        if (!summaryRow.has_value())
        {
            return false;
        }

        const int memberCount = static_cast<int>(m_threads[*threadIndex].members.size());
        if (memberCount <= 0)
        {
            if (const auto existingSummaryRow = visibleSummaryRowForThread(*threadIndex);
                existingSummaryRow.has_value())
            {
                const QModelIndex summaryIndex = index(*existingSummaryRow, 0);
                Q_EMIT dataChanged(summaryIndex, summaryIndex,
                                   {IsExpandedRole, CanExpandRole, ThreadMessageCountRole});
            }
            return true;
        }

        beginRemoveRows(QModelIndex{}, *summaryRow + 1, *summaryRow + memberCount);
        m_rows.erase(m_rows.begin() + (*summaryRow + 1),
                     m_rows.begin() + (*summaryRow + 1 + memberCount));
        endRemoveRows();
        const QModelIndex summaryIndex = index(*summaryRow, 0);
        Q_EMIT dataChanged(summaryIndex, summaryIndex,
                           {IsExpandedRole, CanExpandRole, ThreadMessageCountRole});
        return true;
    }

    bool MessageListModel::toggleThreadExpanded(const std::string_view threadId)
    {
        return setThreadExpanded(threadId, !isThreadExpanded(threadId));
    }

    bool MessageListModel::isThreadExpanded(const std::string_view threadId) const
    {
        return containsThreadId(m_expandedThreadIds, threadId);
    }

    std::optional<std::string>
    MessageListModel::summaryEmailIdForThread(const std::string_view threadId) const
    {
        const auto threadIndex = findThreadIndex(threadId);
        if (!threadIndex.has_value())
        {
            return std::nullopt;
        }

        return m_threads[*threadIndex].summary.emailId;
    }

    bool MessageListModel::isSearchMode() const
    {
        return m_searchQuery.has_value();
    }

    const std::optional<std::string>& MessageListModel::searchQuery() const
    {
        return m_searchQuery;
    }

    const javelin::jmap::cache::MessageListItem&
    MessageListModel::itemForRow(const VisibleRow& row) const
    {
        if (row.kind == RowKind::ThreadSummary)
        {
            return m_threads[row.threadIndex].summary;
        }

        return m_threads[row.threadIndex].members[*row.memberIndex];
    }

    std::optional<std::size_t>
    MessageListModel::findThreadIndex(const std::string_view threadId) const
    {
        for (std::size_t index = 0; index < m_threads.size(); ++index)
        {
            if (m_threads[index].summary.threadId == threadId)
            {
                return index;
            }
        }

        return std::nullopt;
    }

    std::optional<int>
    MessageListModel::visibleSummaryRowForThread(const std::size_t threadIndex) const
    {
        for (std::size_t rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex)
        {
            const auto& row = m_rows[rowIndex];
            if (row.kind == RowKind::ThreadSummary && row.threadIndex == threadIndex)
            {
                return static_cast<int>(rowIndex);
            }
        }

        return std::nullopt;
    }

    bool MessageListModel::loadThreadMembers(const std::size_t threadIndex)
    {
        auto& thread = m_threads[threadIndex];
        if (thread.membersLoaded || !m_accountId.has_value())
        {
            return thread.membersLoaded;
        }

        const auto result =
            m_queryService.listThreadMessages(*m_accountId, thread.summary.threadId);
        const auto* items =
            std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&result);
        if (items == nullptr)
        {
            return false;
        }

        thread.members.clear();
        thread.members.reserve(items->size());
        for (const auto& item : *items)
        {
            if (item.emailId == thread.summary.emailId)
            {
                continue;
            }

            thread.members.push_back(item);
        }
        thread.membersLoaded = true;
        return true;
    }

    void MessageListModel::rebuildVisibleRows()
    {
        m_rows.clear();
        for (std::size_t threadIndex = 0; threadIndex < m_threads.size(); ++threadIndex)
        {
            m_rows.push_back(VisibleRow{
                .kind = RowKind::ThreadSummary,
                .threadIndex = threadIndex,
                .memberIndex = std::nullopt,
            });

            if (!isThreadExpanded(m_threads[threadIndex].summary.threadId))
            {
                continue;
            }

            if (!loadThreadMembers(threadIndex))
            {
                continue;
            }

            for (std::size_t memberIndex = 0; memberIndex < m_threads[threadIndex].members.size();
                 ++memberIndex)
            {
                m_rows.push_back(VisibleRow{
                    .kind = RowKind::ThreadMember,
                    .threadIndex = threadIndex,
                    .memberIndex = memberIndex,
                });
            }
        }
    }

    void MessageListModel::reload()
    {
        std::vector<ThreadEntry> threads;
        std::vector<std::string> survivingExpandedThreadIds;

        if (m_accountId.has_value() && m_mailboxId.has_value() && !m_searchQuery.has_value())
        {
            const auto result = m_queryService.listMailboxMessages(*m_accountId, *m_mailboxId, 100);
            if (const auto* items =
                    std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&result))
            {
                threads.reserve(items->size());
                for (const auto& item : *items)
                {
                    ThreadEntry entry{
                        .summary = item,
                        .members = {},
                        .membersLoaded = false,
                    };
                    if (containsThreadId(m_expandedThreadIds, item.threadId))
                    {
                        survivingExpandedThreadIds.push_back(item.threadId);
                    }
                    threads.push_back(std::move(entry));
                }
            }
        }

        beginResetModel();
        m_threads = std::move(threads);
        m_expandedThreadIds = std::move(survivingExpandedThreadIds);
        rebuildVisibleRows();
        endResetModel();
    }

} // namespace javelin::gui::messages
