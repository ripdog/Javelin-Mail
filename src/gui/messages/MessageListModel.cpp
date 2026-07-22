#include "gui/messages/MessageListModel.h"

#include <QDataStream>
#include <QMimeData>
#include <QString>
#include <QStringList>

#include <algorithm>

namespace javelin::gui::messages
{
    namespace
    {
        constexpr auto emailDragMimeType = "application/x-javelin-mail-email-ids";

        [[nodiscard]] bool containsThreadId(const std::vector<std::string>& threadIds,
                                            const std::string_view threadId)
        {
            return std::ranges::find(threadIds, threadId) != threadIds.end();
        }

        [[nodiscard]] bool
        sameAddress(const std::optional<javelin::jmap::domain::EmailAddress>& left,
                    const std::optional<javelin::jmap::domain::EmailAddress>& right)
        {
            if (left.has_value() != right.has_value())
            {
                return false;
            }

            if (!left.has_value())
            {
                return true;
            }

            return left->name == right->name && left->email == right->email;
        }

        [[nodiscard]] bool sameItem(const javelin::jmap::cache::MessageListItem& left,
                                    const javelin::jmap::cache::MessageListItem& right)
        {
            return left.emailId == right.emailId && left.threadId == right.threadId &&
                   left.subject == right.subject && left.preview == right.preview &&
                   left.receivedAt == right.receivedAt && left.sentAt == right.sentAt &&
                   left.threadMessageCount == right.threadMessageCount &&
                   left.hasAttachment == right.hasAttachment && left.isUnread == right.isUnread &&
                   left.isFlagged == right.isFlagged && sameAddress(left.from, right.from) &&
                   left.mailboxNames == right.mailboxNames;
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

        if (role == SenderEmailRole)
        {
            return item.from.has_value() ? QString::fromStdString(item.from->email) : QString{};
        }

        if (role == SenderNameRole)
        {
            return item.from.has_value() && item.from->name.has_value()
                       ? QString::fromStdString(*item.from->name)
                       : QString{};
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

        if (role == MailboxNamesRole)
        {
            QStringList names;
            names.reserve(static_cast<qsizetype>(item.mailboxNames.size()));
            for (const auto& name : item.mailboxNames)
            {
                names.push_back(QString::fromStdString(name));
            }
            return names;
        }

        if (role == IsSearchResultRole)
        {
            return !m_mailboxId.has_value();
        }

        return {};
    }

    Qt::ItemFlags MessageListModel::flags(const QModelIndex& index) const
    {
        auto result = QAbstractListModel::flags(index);
        if (index.isValid())
        {
            result |= Qt::ItemIsDragEnabled;
        }
        return result;
    }

    QStringList MessageListModel::mimeTypes() const
    {
        return {QString::fromLatin1(emailDragMimeType)};
    }

    QMimeData* MessageListModel::mimeData(const QModelIndexList& indexes) const
    {
        if (!m_accountId.has_value())
        {
            return nullptr;
        }

        QStringList emailIds;
        for (const auto& index : indexes)
        {
            const auto emailId = data(index, EmailIdRole).toString();
            if (!emailId.isEmpty() && !emailIds.contains(emailId))
            {
                emailIds.push_back(emailId);
            }
        }
        if (emailIds.isEmpty())
        {
            return nullptr;
        }

        QByteArray payload;
        QDataStream stream{&payload, QIODeviceBase::WriteOnly};
        stream << QString::fromStdString(*m_accountId) << emailIds;
        auto* mimeData = new QMimeData;
        mimeData->setData(QString::fromLatin1(emailDragMimeType), payload);
        return mimeData;
    }

    Qt::DropActions MessageListModel::supportedDragActions() const
    {
        return Qt::MoveAction;
    }

    void MessageListModel::setPage(std::optional<std::string> accountId,
                                   std::optional<std::string> mailboxId,
                                   std::vector<javelin::jmap::cache::MessageListItem> items)
    {
        // A disjoint query page is one logical replacement. Publishing it as a long sequence of
        // row removals and insertions makes QItemSelectionModel visit transient row identities.
        const bool pagesAreDisjoint =
            (!m_threads.empty() || !items.empty()) &&
            std::ranges::none_of(items, [this](const auto& item)
                                 { return findThreadIndex(item.threadId).has_value(); });
        if (m_accountId != accountId || m_mailboxId != mailboxId || !m_expandedThreadIds.empty() ||
            pagesAreDisjoint)
        {
            beginResetModel();
            m_accountId = std::move(accountId);
            m_mailboxId = std::move(mailboxId);
            m_threads.clear();
            m_threads.reserve(items.size());

            std::vector<std::string> survivingExpandedThreadIds;
            survivingExpandedThreadIds.reserve(m_expandedThreadIds.size());
            for (auto& item : items)
            {
                if (containsThreadId(m_expandedThreadIds, item.threadId))
                {
                    survivingExpandedThreadIds.push_back(item.threadId);
                }

                m_threads.push_back(ThreadEntry{
                    .summary = std::move(item),
                    .members = {},
                    .membersLoaded = false,
                });
            }
            m_expandedThreadIds = std::move(survivingExpandedThreadIds);
            rebuildVisibleRows();
            endResetModel();
            return;
        }

        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);

        for (int threadIndex = static_cast<int>(m_threads.size()) - 1; threadIndex >= 0;
             --threadIndex)
        {
            const auto threadIndexValue = static_cast<std::size_t>(threadIndex);
            const auto existsInNewPage = std::ranges::any_of(
                items, [threadId = m_threads[threadIndexValue].summary.threadId](const auto& item)
                { return item.threadId == threadId; });
            if (existsInNewPage)
            {
                continue;
            }

            const int blockStart =
                visibleBlockStartForThread(static_cast<std::size_t>(threadIndex));
            const int blockSize = visibleBlockSizeForThread(static_cast<std::size_t>(threadIndex));
            beginRemoveRows(QModelIndex{}, blockStart, blockStart + blockSize - 1);
            m_rows.erase(m_rows.begin() + blockStart, m_rows.begin() + blockStart + blockSize);
            m_threads.erase(m_threads.begin() + threadIndex);
            reindexVisibleRows();
            endRemoveRows();
        }

        for (std::size_t targetIndex = 0; targetIndex < items.size(); ++targetIndex)
        {
            auto existingIndex = findThreadIndex(items[targetIndex].threadId);
            if (!existingIndex.has_value())
            {
                beginInsertRows(QModelIndex{}, static_cast<int>(targetIndex),
                                static_cast<int>(targetIndex));
                m_threads.insert(m_threads.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                                 ThreadEntry{
                                     .summary = std::move(items[targetIndex]),
                                     .members = {},
                                     .membersLoaded = false,
                                 });
                m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                              VisibleRow{
                                  .kind = RowKind::ThreadSummary,
                                  .threadIndex = targetIndex,
                                  .memberIndex = std::nullopt,
                              });
                reindexVisibleRows();
                endInsertRows();
                continue;
            }

            if (*existingIndex != targetIndex)
            {
                beginMoveRows(QModelIndex{}, static_cast<int>(*existingIndex),
                              static_cast<int>(*existingIndex), QModelIndex{},
                              *existingIndex < targetIndex ? static_cast<int>(targetIndex + 1)
                                                           : static_cast<int>(targetIndex));
                auto thread = std::move(m_threads[*existingIndex]);
                m_threads.erase(m_threads.begin() + static_cast<std::ptrdiff_t>(*existingIndex));
                m_threads.insert(m_threads.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                                 std::move(thread));
                auto row = std::move(m_rows[*existingIndex]);
                m_rows.erase(m_rows.begin() + static_cast<std::ptrdiff_t>(*existingIndex));
                m_rows.insert(m_rows.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                              std::move(row));
                reindexVisibleRows();
                endMoveRows();
                existingIndex = targetIndex;
            }

            if (!sameItem(m_threads[*existingIndex].summary, items[targetIndex]))
            {
                m_threads[*existingIndex].summary = std::move(items[targetIndex]);
                const QModelIndex changed = index(static_cast<int>(*existingIndex), 0);
                Q_EMIT dataChanged(changed, changed);
            }
        }
    }

    void MessageListModel::clear()
    {
        setPage(std::nullopt, std::nullopt, {});
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
            m_mailboxId.has_value()
                ? m_queryService.listMailboxThreadMessages(*m_accountId, *m_mailboxId,
                                                           thread.summary.threadId)
                : m_queryService.listThreadMessages(*m_accountId, thread.summary.threadId);
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

    int MessageListModel::visibleBlockStartForThread(const std::size_t threadIndex) const
    {
        const auto summaryRow = visibleSummaryRowForThread(threadIndex);
        return summaryRow.value_or(static_cast<int>(threadIndex));
    }

    int MessageListModel::visibleBlockSizeForThread(const std::size_t threadIndex) const
    {
        if (!isThreadExpanded(m_threads[threadIndex].summary.threadId))
        {
            return 1;
        }

        return 1 + static_cast<int>(m_threads[threadIndex].members.size());
    }

    void MessageListModel::reindexVisibleRows()
    {
        for (std::size_t rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex)
        {
            m_rows[rowIndex].threadIndex = rowIndex;
        }
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

} // namespace javelin::gui::messages
