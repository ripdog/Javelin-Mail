#include "gui/messages/MessageListModel.h"

#include "app/MessageListCacheRead.h"
#include "app/MessageSubject.h"

#include <KLocalizedString>

#include <QDataStream>
#include <QDateTime>
#include <QFutureWatcher>
#include <QLocale>
#include <QMimeData>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtConcurrentRun>

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
                   left.bodyPreview == right.bodyPreview && left.receivedAt == right.receivedAt &&
                   left.sentAt == right.sentAt &&
                   left.mailboxThreadMessageCount == right.mailboxThreadMessageCount &&
                   left.globalThreadMessageCount == right.globalThreadMessageCount &&
                   left.hasAttachment == right.hasAttachment && left.isUnread == right.isUnread &&
                   left.isFlagged == right.isFlagged && left.isJunk == right.isJunk &&
                   sameAddress(left.from, right.from) && left.mailboxNames == right.mailboxNames &&
                   left.tags == right.tags;
        }

        [[nodiscard]] QString formattedTimestamp(const std::string_view isoTimestamp)
        {
            const auto value =
                QString::fromUtf8(isoTimestamp.data(), static_cast<qsizetype>(isoTimestamp.size()));
            const auto dateTime = QDateTime::fromString(value, Qt::ISODate);
            return dateTime.isValid()
                       ? QLocale{}.toString(dateTime.toLocalTime(), QLocale::ShortFormat)
                       : value;
        }

    } // namespace

    MessageListModel::MessageListModel(javelin::jmap::cache::QueryReader& queryReader,
                                       QObject* parent)
        : QAbstractListModel(parent), m_queryReader(queryReader)
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
                                : i18nc("@item email with unknown sender", "(unknown sender)");
        const auto subject = javelin::app::subjectForDisplay(item.subject);
        const auto preview =
            item.preview.has_value() ? QString::fromStdString(*item.preview) : QString{};
        const auto tooltipPreview =
            (item.bodyPreview.has_value() ? QString::fromStdString(*item.bodyPreview) : preview)
                .simplified();

        if (role == Qt::DisplayRole)
        {
            return QStringLiteral("%1  %2").arg(sender, subject);
        }

        if (role == Qt::ToolTipRole)
        {
            return tooltipPreview;
        }

        if (role == Qt::AccessibleTextRole)
        {
            QStringList parts{sender, subject};
            if (!item.receivedAt.empty())
                parts.push_back(formattedTimestamp(item.receivedAt));
            if (item.isUnread)
                parts.push_back(i18nc("@info accessible message state", "Unread"));
            if (item.isFlagged)
                parts.push_back(i18nc("@info accessible message state", "Starred"));
            if (item.hasAttachment)
                parts.push_back(i18nc("@info accessible message state", "Has attachment"));
            const bool canExpand = item.mailboxThreadMessageCount.value_or(0) > 1 ||
                                   item.globalThreadMessageCount.value_or(0) > 1;
            if (row.kind == RowKind::ThreadSummary && canExpand)
            {
                if (item.mailboxThreadMessageCount.has_value())
                {
                    parts.push_back(i18np("%1 message in this mailbox",
                                          "%1 messages in this mailbox",
                                          *item.mailboxThreadMessageCount));
                }
                else
                {
                    parts.push_back(i18nc("@info accessible conversation", "Conversation"));
                }
                parts.push_back(isThreadExpanded(item.threadId)
                                    ? i18nc("@info accessible conversation state", "Expanded")
                                    : i18nc("@info accessible conversation state", "Collapsed"));
            }
            if (!item.tags.empty())
            {
                QStringList tagNames;
                tagNames.reserve(static_cast<qsizetype>(item.tags.size()));
                for (const auto& tag : item.tags)
                    tagNames.push_back(tag.displayName);
                parts.push_back(i18nc("@info accessible message tags", "Tags: %1",
                                      tagNames.join(QStringLiteral(", "))));
            }
            if (!m_mailboxId.has_value() && !item.mailboxNames.empty())
            {
                QStringList mailboxNames;
                mailboxNames.reserve(static_cast<qsizetype>(item.mailboxNames.size()));
                for (const auto& name : item.mailboxNames)
                    mailboxNames.push_back(QString::fromStdString(name));
                parts.push_back(i18nc("@info accessible message mailboxes", "Mailboxes: %1",
                                      mailboxNames.join(QStringLiteral(", "))));
            }
            return parts.join(QStringLiteral(", "));
        }

        if (role == Qt::AccessibleDescriptionRole)
        {
            return tooltipPreview.isEmpty() ? QVariant{}
                                            : QVariant{i18nc("@info accessible message preview",
                                                             "Preview: %1", tooltipPreview)};
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

        if (role == IsJunkRole)
        {
            return item.isJunk;
        }

        if (role == ThreadMessageCountRole)
        {
            return item.mailboxThreadMessageCount.has_value()
                       ? QVariant{static_cast<qulonglong>(*item.mailboxThreadMessageCount)}
                       : QVariant{};
        }

        if (role == GlobalThreadMessageCountRole)
        {
            return item.globalThreadMessageCount.has_value()
                       ? QVariant{static_cast<qulonglong>(*item.globalThreadMessageCount)}
                       : QVariant{};
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
            return row.kind == RowKind::ThreadSummary &&
                   (item.mailboxThreadMessageCount.value_or(0) > 1 ||
                    item.globalThreadMessageCount.value_or(0) > 1);
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

        if (role == TagNamesRole || role == TagColorsRole)
        {
            QStringList values;
            values.reserve(static_cast<qsizetype>(item.tags.size()));
            for (const auto& tag : item.tags)
                values.push_back(role == TagNamesRole ? tag.displayName : tag.color);
            return values;
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

    void MessageListModel::setItems(std::optional<std::string> accountId,
                                    std::optional<std::string> mailboxId,
                                    const std::vector<javelin::jmap::cache::MessageListItem>& items)
    {
        const bool sameContext = m_accountId == accountId && m_mailboxId == mailboxId;
        const bool extendsCurrentPrefix =
            sameContext && items.size() >= m_threads.size() &&
            std::equal(m_threads.begin(), m_threads.end(), items.begin(),
                       [](const ThreadEntry& existing, const auto& replacement)
                       { return existing.summary.threadId == replacement.threadId; });
        if (extendsCurrentPrefix)
        {
            m_accountId = std::move(accountId);
            m_mailboxId = std::move(mailboxId);
            for (std::size_t threadIndex = 0; threadIndex < m_threads.size(); ++threadIndex)
            {
                if (!sameItem(m_threads[threadIndex].summary, items[threadIndex]))
                {
                    m_threads[threadIndex].summary = items[threadIndex];
                    if (const auto row = visibleSummaryRowForThread(threadIndex); row.has_value())
                    {
                        const QModelIndex changed = index(*row, 0);
                        Q_EMIT dataChanged(changed, changed);
                    }
                }
            }

            if (items.size() > m_threads.size())
            {
                const auto firstThread = m_threads.size();
                const auto addedCount = items.size() - firstThread;
                const int firstRow = static_cast<int>(m_rows.size());
                const int lastRow = firstRow + static_cast<int>(addedCount) - 1;
                beginInsertRows(QModelIndex{}, firstRow, lastRow);
                m_threads.reserve(items.size());
                m_rows.reserve(m_rows.size() + addedCount);
                for (std::size_t threadIndex = firstThread; threadIndex < items.size();
                     ++threadIndex)
                {
                    m_threads.push_back(ThreadEntry{
                        .summary = items[threadIndex],
                        .members = {},
                        .membersLoaded = false,
                        .membersLoading = false,
                    });
                    m_rows.push_back(VisibleRow{
                        .kind = RowKind::ThreadSummary,
                        .threadIndex = threadIndex,
                        .memberIndex = std::nullopt,
                    });
                }
                endInsertRows();
            }
            retryPendingThreadMembersLoads();
            return;
        }

        ++m_generation;
        // A disjoint query result is one logical replacement. Publishing it as a long sequence of
        // row removals and insertions makes QItemSelectionModel visit transient row identities.
        const bool listsAreDisjoint =
            (!m_threads.empty() || !items.empty()) &&
            std::ranges::none_of(items, [this](const auto& item)
                                 { return findThreadIndex(item.threadId).has_value(); });
        if (!sameContext || !m_expandedThreadIds.empty() || listsAreDisjoint)
        {
            beginResetModel();
            m_accountId = std::move(accountId);
            m_mailboxId = std::move(mailboxId);
            m_threads.clear();
            m_threads.reserve(items.size());

            std::vector<std::string> survivingExpandedThreadIds;
            survivingExpandedThreadIds.reserve(m_expandedThreadIds.size());
            for (const auto& item : items)
            {
                if (containsThreadId(m_expandedThreadIds, item.threadId))
                    survivingExpandedThreadIds.push_back(item.threadId);

                m_threads.push_back(ThreadEntry{
                    .summary = std::move(item),
                    .members = {},
                    .membersLoaded = false,
                    .membersLoading = false,
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
            const auto existsInNewList = std::ranges::any_of(
                items, [threadId = m_threads[threadIndexValue].summary.threadId](const auto& item)
                { return item.threadId == threadId; });
            if (existsInNewList)
                continue;

            const int blockStart = visibleBlockStartForThread(threadIndexValue);
            const int blockSize = visibleBlockSizeForThread(threadIndexValue);
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
                                     .summary = items[targetIndex],
                                     .members = {},
                                     .membersLoaded = false,
                                     .membersLoading = false,
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
                m_threads[*existingIndex].summary = items[targetIndex];
                const QModelIndex changed = index(static_cast<int>(*existingIndex), 0);
                Q_EMIT dataChanged(changed, changed);
            }
        }
    }

    void MessageListModel::clear()
    {
        setItems(std::nullopt, std::nullopt, {});
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
            m_expandedThreadIds.push_back(std::string{threadId});
            auto& thread = m_threads[*threadIndex];
            if (!thread.membersLoaded)
            {
                startThreadMembersLoad(*threadIndex);
                const auto summaryRow = visibleSummaryRowForThread(*threadIndex);
                if (summaryRow.has_value())
                {
                    const QModelIndex summaryIndex = index(*summaryRow, 0);
                    Q_EMIT dataChanged(summaryIndex, summaryIndex,
                                       {IsExpandedRole, CanExpandRole, ThreadMessageCountRole,
                                        GlobalThreadMessageCountRole, Qt::AccessibleTextRole});
                }
                return true;
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
                               {IsExpandedRole, CanExpandRole, ThreadMessageCountRole,
                                GlobalThreadMessageCountRole, Qt::AccessibleTextRole});
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
                                   {IsExpandedRole, CanExpandRole, ThreadMessageCountRole,
                                    GlobalThreadMessageCountRole, Qt::AccessibleTextRole});
            }
            return true;
        }

        beginRemoveRows(QModelIndex{}, *summaryRow + 1, *summaryRow + memberCount);
        m_rows.erase(m_rows.begin() + (*summaryRow + 1),
                     m_rows.begin() + (*summaryRow + 1 + memberCount));
        endRemoveRows();
        const QModelIndex summaryIndex = index(*summaryRow, 0);
        Q_EMIT dataChanged(summaryIndex, summaryIndex,
                           {IsExpandedRole, CanExpandRole, ThreadMessageCountRole,
                            GlobalThreadMessageCountRole, Qt::AccessibleTextRole});
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

    bool MessageListModel::setEmailRead(const std::string_view emailId)
    {
        bool changed = false;
        for (auto& thread : m_threads)
        {
            if (thread.summary.emailId == emailId && thread.summary.isUnread)
            {
                thread.summary.isUnread = false;
                changed = true;
            }
            for (auto& member : thread.members)
            {
                if (member.emailId == emailId && member.isUnread)
                {
                    member.isUnread = false;
                    changed = true;
                }
            }
        }
        if (!changed)
            return false;

        for (std::size_t row = 0; row < m_rows.size(); ++row)
        {
            if (itemForRow(m_rows[row]).emailId == emailId)
            {
                const auto changedIndex = index(static_cast<int>(row), 0);
                Q_EMIT dataChanged(changedIndex, changedIndex,
                                   {IsUnreadRole, Qt::AccessibleTextRole});
            }
        }
        return true;
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

    void MessageListModel::startThreadMembersLoad(const std::size_t requestedThreadIndex)
    {
        auto& thread = m_threads[requestedThreadIndex];
        if (thread.membersLoaded || thread.membersLoading || !m_accountId.has_value())
        {
            return;
        }
        thread.membersLoading = true;
        const auto generation = m_generation;
        const auto threadId = thread.summary.threadId;
        const auto accountId = *m_accountId;
        const auto mailboxId = m_mailboxId;
        auto* watcher = new QFutureWatcher<javelin::app::MessageListThreadMembersResult>(this);
        connect(
            watcher, &QFutureWatcher<javelin::app::MessageListThreadMembersResult>::finished, this,
            [this, watcher, generation, threadId]
            {
                auto result = watcher->result();
                watcher->deleteLater();
                if (generation != m_generation)
                    return;
                const auto resolvedThreadIndex = findThreadIndex(threadId);
                if (!resolvedThreadIndex.has_value())
                    return;
                auto& loadedThread = m_threads[*resolvedThreadIndex];
                loadedThread.membersLoading = false;
                const auto* snapshot =
                    std::get_if<javelin::app::MessageListThreadMembersSnapshot>(&result);
                if (snapshot == nullptr)
                {
                    constexpr std::uint8_t maximumLocalReadRetries = 2;
                    if (loadedThread.memberReadFailureCount < maximumLocalReadRetries)
                    {
                        ++loadedThread.memberReadFailureCount;
                        QTimer::singleShot(
                            50, this,
                            [this, generation, threadId]
                            {
                                if (generation != m_generation || !isThreadExpanded(threadId))
                                    return;
                                const auto retryThreadIndex = findThreadIndex(threadId);
                                if (retryThreadIndex.has_value())
                                    startThreadMembersLoad(*retryThreadIndex);
                            });
                    }
                    return;
                }
                loadedThread.memberReadFailureCount = 0;
                if (!snapshot->complete)
                {
                    if (isThreadExpanded(threadId))
                        Q_EMIT threadMaterializationRequired(QString::fromStdString(threadId));
                    return;
                }

                loadedThread.members.clear();
                loadedThread.members.reserve(snapshot->items.size());
                for (const auto& item : snapshot->items)
                {
                    if (item.emailId != loadedThread.summary.emailId)
                        loadedThread.members.push_back(item);
                }
                loadedThread.membersLoaded = true;
                if (isThreadExpanded(threadId) && !loadedThread.members.empty())
                {
                    const auto summaryRow = visibleSummaryRowForThread(*resolvedThreadIndex);
                    if (summaryRow.has_value())
                    {
                        const auto memberCount = static_cast<int>(loadedThread.members.size());
                        beginInsertRows(QModelIndex{}, *summaryRow + 1, *summaryRow + memberCount);
                        for (int memberRow = 0; memberRow < memberCount; ++memberRow)
                        {
                            m_rows.insert(m_rows.begin() + (*summaryRow + 1 + memberRow),
                                          VisibleRow{
                                              .kind = RowKind::ThreadMember,
                                              .threadIndex = *resolvedThreadIndex,
                                              .memberIndex = static_cast<std::size_t>(memberRow),
                                          });
                        }
                        endInsertRows();
                    }
                }
                if (const auto summaryRow = visibleSummaryRowForThread(*resolvedThreadIndex);
                    summaryRow.has_value())
                {
                    const QModelIndex summaryIndex = index(*summaryRow, 0);
                    Q_EMIT dataChanged(summaryIndex, summaryIndex,
                                       {IsExpandedRole, CanExpandRole, ThreadMessageCountRole,
                                        GlobalThreadMessageCountRole, Qt::AccessibleTextRole});
                }
            });
        watcher->setFuture(QtConcurrent::run(javelin::app::loadMessageListThreadMembers,
                                             m_queryReader.databasePath(), accountId, mailboxId,
                                             threadId));
    }

    void MessageListModel::retryPendingThreadMembersLoads()
    {
        const bool hasPending = std::ranges::any_of(
            m_threads, [this](const ThreadEntry& thread)
            { return !thread.membersLoaded && isThreadExpanded(thread.summary.threadId); });
        if (!hasPending)
            return;

        ++m_generation;
        for (std::size_t threadIndex = 0; threadIndex < m_threads.size(); ++threadIndex)
        {
            auto& thread = m_threads[threadIndex];
            if (thread.membersLoaded || !isThreadExpanded(thread.summary.threadId))
                continue;
            thread.membersLoading = false;
            startThreadMembersLoad(threadIndex);
        }
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

            auto& thread = m_threads[threadIndex];
            if (!thread.membersLoaded)
            {
                startThreadMembersLoad(threadIndex);
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
