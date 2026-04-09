#include "gui/mailboxes/MailboxTreeModel.h"

#include <QIcon>

#include <unordered_map>

namespace javelin::gui::mailboxes
{

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::AccountRepository& accountRepository,
                                       javelin::jmap::cache::QueryService& queryService,
                                       QObject* parent)
        : QAbstractItemModel(parent), m_accountRepository(accountRepository),
          m_queryService(queryService)
    {
        rebuild();
    }

    MailboxTreeModel::~MailboxTreeModel() = default;

    QModelIndex MailboxTreeModel::index(const int row, const int column,
                                        const QModelIndex& parentIndex) const
    {
        if (column != 0 || row < 0)
        {
            return {};
        }

        if (!parentIndex.isValid())
        {
            if (static_cast<std::size_t>(row) >= m_rootNodes.size())
            {
                return {};
            }

            return createIndex(row, column, m_rootNodes[static_cast<std::size_t>(row)].get());
        }

        const auto* parentNode = static_cast<const Node*>(parentIndex.internalPointer());
        if (parentNode == nullptr || static_cast<std::size_t>(row) >= parentNode->children.size())
        {
            return {};
        }

        return createIndex(row, column, parentNode->children[static_cast<std::size_t>(row)].get());
    }

    QModelIndex MailboxTreeModel::parent(const QModelIndex& child) const
    {
        if (!child.isValid())
        {
            return {};
        }

        const auto* node = static_cast<const Node*>(child.internalPointer());
        if (node == nullptr || node->parent == nullptr)
        {
            return {};
        }

        const auto* grandParent = node->parent->parent;
        const auto& siblings = grandParent == nullptr ? m_rootNodes : grandParent->children;
        for (std::size_t idx = 0; idx < siblings.size(); ++idx)
        {
            if (siblings[idx].get() == node->parent)
            {
                return createIndex(static_cast<int>(idx), 0, node->parent);
            }
        }

        return {};
    }

    int MailboxTreeModel::rowCount(const QModelIndex& parentIndex) const
    {
        if (parentIndex.column() > 0)
        {
            return 0;
        }

        if (!parentIndex.isValid())
        {
            return static_cast<int>(m_rootNodes.size());
        }

        const auto* parentNode = static_cast<const Node*>(parentIndex.internalPointer());
        return parentNode == nullptr ? 0 : static_cast<int>(parentNode->children.size());
    }

    int MailboxTreeModel::columnCount(const QModelIndex& parentIdx) const
    {
        Q_UNUSED(parentIdx);
        return 1;
    }

    QVariant MailboxTreeModel::data(const QModelIndex& index, const int role) const
    {
        const auto* node = nodeForIndex(index);
        if (node == nullptr)
        {
            return {};
        }

        if (role == Qt::DisplayRole)
        {
            if (!node->mailboxId.empty() && node->unreadEmails > 0)
            {
                return QStringLiteral("%1 (%2)")
                    .arg(QString::fromStdString(node->displayName))
                    .arg(node->unreadEmails);
            }
            return QString::fromStdString(node->displayName);
        }

        if (role == Qt::ToolTipRole)
        {
            if (!node->mailboxId.empty())
            {
                return QStringLiteral("%1 (%2 unread)")
                    .arg(QString::fromStdString(node->displayName))
                    .arg(node->unreadEmails);
            }
            return QString::fromStdString(node->displayName);
        }

        if (role == MailboxIdRole)
        {
            // Returns empty string for account-level nodes (selecting an account is not a
            // mailbox selection).
            return QString::fromStdString(node->mailboxId);
        }

        if (role == AccountIdRole)
        {
            return QString::fromStdString(node->accountId);
        }

        if (role == TotalThreadsRole)
        {
            return static_cast<qulonglong>(node->totalThreads);
        }

        return {};
    }

    void MailboxTreeModel::refresh()
    {
        rebuild();
    }

    const MailboxTreeModel::Node* MailboxTreeModel::nodeForIndex(const QModelIndex& index) const
    {
        return index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
    }

    void MailboxTreeModel::rebuild()
    {
        beginResetModel();
        m_rootNodes.clear();

        const auto accountsResult = m_accountRepository.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
        {
            endResetModel();
            return;
        }

        for (const auto& account : *accounts)
        {
            auto accountNode = std::make_unique<Node>();
            accountNode->accountId = account.accountId;
            accountNode->displayName = account.name.empty() ? account.accountId : account.name;
            // mailboxId left empty — this is an account-level node.

            const auto mailboxResult = m_queryService.listMailboxTree(account.accountId);
            const auto* mailboxItems =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxResult);

            if (mailboxItems != nullptr)
            {
                std::unordered_map<std::string, Node*> nodesById;
                std::vector<javelin::jmap::cache::MailboxTreeItem> roots;
                std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
                    deferredChildren;

                auto attachChildren = [&nodesById, &deferredChildren,
                                       &accountId = account.accountId](const auto& self,
                                                                       Node* parentNode) -> void
                {
                    const auto deferredIt = deferredChildren.find(parentNode->mailboxId);
                    if (deferredIt == deferredChildren.end())
                    {
                        return;
                    }

                    auto children = std::move(deferredIt->second);
                    deferredChildren.erase(deferredIt);
                    for (auto& childItem : children)
                    {
                        auto child = std::make_unique<Node>();
                        child->accountId = accountId;
                        child->displayName = childItem.name;
                        child->mailboxId = childItem.id;
                        child->unreadEmails = childItem.unreadEmails;
                        child->totalThreads = childItem.totalThreads;
                        child->parent = parentNode;
                        nodesById.emplace(child->mailboxId, child.get());
                        parentNode->children.push_back(std::move(child));
                        self(self, parentNode->children.back().get());
                    }
                };

                for (const auto& item : *mailboxItems)
                {
                    if (!item.parentId.has_value())
                    {
                        roots.push_back(item);
                        continue;
                    }

                    const auto parentIt = nodesById.find(*item.parentId);
                    if (parentIt == nodesById.end())
                    {
                        deferredChildren[*item.parentId].push_back(item);
                        continue;
                    }

                    auto child = std::make_unique<Node>();
                    child->accountId = account.accountId;
                    child->displayName = item.name;
                    child->mailboxId = item.id;
                    child->unreadEmails = item.unreadEmails;
                    child->totalThreads = item.totalThreads;
                    child->parent = parentIt->second;
                    nodesById.emplace(child->mailboxId, child.get());
                    parentIt->second->children.push_back(std::move(child));
                    attachChildren(attachChildren, parentIt->second->children.back().get());
                }

                for (auto& rootItem : roots)
                {
                    auto rootMailboxNode = std::make_unique<Node>();
                    rootMailboxNode->accountId = account.accountId;
                    rootMailboxNode->displayName = rootItem.name;
                    rootMailboxNode->mailboxId = rootItem.id;
                    rootMailboxNode->unreadEmails = rootItem.unreadEmails;
                    rootMailboxNode->totalThreads = rootItem.totalThreads;
                    rootMailboxNode->parent = accountNode.get();
                    nodesById.emplace(rootMailboxNode->mailboxId, rootMailboxNode.get());
                    accountNode->children.push_back(std::move(rootMailboxNode));
                    attachChildren(attachChildren, accountNode->children.back().get());
                }
            }

            m_rootNodes.push_back(std::move(accountNode));
        }

        endResetModel();
    }

} // namespace javelin::gui::mailboxes
