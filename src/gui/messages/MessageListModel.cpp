#include "gui/messages/MessageListModel.h"

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
        if (role == Qt::DisplayRole)
        {
            const auto sender = item.from.has_value() ? QString::fromStdString(item.from->email)
                                                      : QStringLiteral("(unknown sender)");
            const auto subject = item.subject.has_value() ? QString::fromStdString(*item.subject)
                                                          : QStringLiteral("(no subject)");
            return QStringLiteral("%1  %2").arg(sender, subject);
        }

        if (role == Qt::ToolTipRole)
        {
            return QString::fromStdString(item.preview.value_or(std::string{}));
        }

        if (role == EmailIdRole)
        {
            return QString::fromStdString(item.emailId);
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
