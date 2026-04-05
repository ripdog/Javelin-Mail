#include "gui/messages/MessageListModel.h"

#include <QDateTime>

namespace javelin::gui::messages
{

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

    void MessageListModel::setMailboxContext(std::optional<std::string> accountId,
                                             std::optional<std::string> mailboxId)
    {
        if (m_accountId == accountId && m_mailboxId == mailboxId)
        {
            return;
        }

        m_accountId = std::move(accountId);
        m_mailboxId = std::move(mailboxId);
        reload();
    }

    void MessageListModel::refresh()
    {
        reload();
    }

    void MessageListModel::reload()
    {
        beginResetModel();
        m_items.clear();

        if (m_accountId.has_value() && m_mailboxId.has_value())
        {
            const auto result = m_queryService.listMailboxMessages(*m_accountId, *m_mailboxId, 100);
            if (const auto* items =
                    std::get_if<std::vector<javelin::jmap::cache::MessageListItem>>(&result))
            {
                m_items = *items;
            }
        }

        endResetModel();
    }

} // namespace javelin::gui::messages
