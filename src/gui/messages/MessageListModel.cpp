#include "gui/messages/MessageListModel.h"

#include <QDateTime>

namespace javelin::gui::messages
{
    namespace
    {

        [[nodiscard]] bool equalEmailAddress(
            const std::optional<javelin::jmap::domain::EmailAddress>& left,
            const std::optional<javelin::jmap::domain::EmailAddress>& right)
        {
            if (!left.has_value() && !right.has_value())
            {
                return true;
            }

            if (left.has_value() != right.has_value())
            {
                return false;
            }

            return left->name == right->name && left->email == right->email;
        }

        [[nodiscard]] bool equalMessageListItem(
            const javelin::jmap::cache::MessageListItem& left,
            const javelin::jmap::cache::MessageListItem& right)
        {
            return left.emailId == right.emailId && left.threadId == right.threadId &&
                   left.subject == right.subject && left.preview == right.preview &&
                   left.receivedAt == right.receivedAt && left.sentAt == right.sentAt &&
                   left.threadMessageCount == right.threadMessageCount &&
                   left.hasAttachment == right.hasAttachment && left.isUnread == right.isUnread &&
                   left.isFlagged == right.isFlagged && equalEmailAddress(left.from, right.from);
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

        return static_cast<int>(m_items.size());
    }

    QVariant MessageListModel::data(const QModelIndex& index, const int role) const
    {
        if (!index.isValid() || index.row() < 0 ||
            static_cast<std::size_t>(index.row()) >= m_items.size())
        {
            return {};
        }

        const auto& item = m_items[static_cast<std::size_t>(index.row())];
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

        return {};
    }

    std::optional<std::size_t> MessageListModel::indexOfThread(const std::string_view threadId) const
    {
        for (std::size_t index = 0; index < m_items.size(); ++index)
        {
            if (m_items[index].threadId == threadId)
            {
                return index;
            }
        }

        return std::nullopt;
    }

    void MessageListModel::setMailboxContext(std::optional<std::string> accountId,
                                             std::optional<std::string> mailboxId)
    {
        if (m_accountId == accountId && m_mailboxId == mailboxId)
        {
            return;
        }

        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        reload(false);
    }

    void MessageListModel::refresh()
    {
        reload(true);
    }

    void MessageListModel::reload(const bool preserveSelection)
    {
        std::vector<javelin::jmap::cache::MessageListItem> items;

        if (m_accountId.has_value() && m_mailboxId.has_value())
        {
            const auto result = m_queryService.listMailboxMessages(*m_accountId, *m_mailboxId, 100);
            if (const auto* loadedItems =
                    std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&result))
            {
                if (!preserveSelection)
                {
                    beginResetModel();
                    m_items = *loadedItems;
                    endResetModel();
                    return;
                }

                applyRefresh(*loadedItems);
                return;
            }
        }

        if (!preserveSelection)
        {
            beginResetModel();
            m_items.clear();
            endResetModel();
            return;
        }

        applyRefresh(items);
    }

    void MessageListModel::applyRefresh(
        const std::vector<javelin::jmap::cache::MessageListItem>& items)
    {
        for (auto it = m_items.size(); it > 0; --it)
        {
            const auto oldIndex = it - 1;
            const auto threadId = m_items[oldIndex].threadId;
            bool stillPresent = false;
            for (const auto& item : items)
            {
                if (item.threadId == threadId)
                {
                    stillPresent = true;
                    break;
                }
            }

            if (!stillPresent)
            {
                beginRemoveRows(QModelIndex{}, static_cast<int>(oldIndex),
                                static_cast<int>(oldIndex));
                m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(oldIndex));
                endRemoveRows();
            }
        }

        for (std::size_t newIndex = 0; newIndex < items.size(); ++newIndex)
        {
            const auto& item = items[newIndex];
            bool alreadyPresent = false;
            for (const auto& existing : m_items)
            {
                if (existing.threadId == item.threadId)
                {
                    alreadyPresent = true;
                    break;
                }
            }

            if (!alreadyPresent)
            {
                beginInsertRows(QModelIndex{}, static_cast<int>(newIndex),
                                static_cast<int>(newIndex));
                m_items.insert(m_items.begin() + static_cast<std::ptrdiff_t>(newIndex), item);
                endInsertRows();
            }
        }

        for (std::size_t newIndex = 0; newIndex < items.size(); ++newIndex)
        {
            if (newIndex >= m_items.size())
            {
                break;
            }

            if (m_items[newIndex].threadId == items[newIndex].threadId)
            {
                continue;
            }

            const auto currentIndex = indexOfThread(items[newIndex].threadId);
            if (!currentIndex.has_value())
            {
                continue;
            }

            const int sourceRow = static_cast<int>(*currentIndex);
            const int destinationRow =
                *currentIndex > newIndex ? static_cast<int>(newIndex)
                                         : static_cast<int>(newIndex + 1);
            beginMoveRows(QModelIndex{}, sourceRow, sourceRow, QModelIndex{}, destinationRow);
            auto movedItem = std::move(m_items[*currentIndex]);
            m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(*currentIndex));
            m_items.insert(m_items.begin() + static_cast<std::ptrdiff_t>(newIndex),
                           std::move(movedItem));
            endMoveRows();
        }

        for (std::size_t index = 0; index < items.size() && index < m_items.size(); ++index)
        {
            if (!equalMessageListItem(m_items[index], items[index]))
            {
                m_items[index] = items[index];
                const QModelIndex modelIndex = this->index(static_cast<int>(index), 0);
                if (modelIndex.isValid())
                {
                    Q_EMIT dataChanged(modelIndex, modelIndex);
                }
            }
        }
    }

} // namespace javelin::gui::messages
