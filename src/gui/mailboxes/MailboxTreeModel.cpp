#include "gui/mailboxes/MailboxTreeModel.h"

#include <QIcon>

#include <unordered_map>

namespace javelin::gui::mailboxes
{

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::QueryService& queryService,
                                       QObject* parent)
        : QAbstractItemModel(parent), m_queryService(queryService)
    {
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
        for (std::size_t index = 0; index < siblings.size(); ++index)
        {
            if (siblings[index].get() == node->parent)
            {
                return createIndex(static_cast<int>(index), 0, node->parent);
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

    int MailboxTreeModel::columnCount(const QModelIndex& parent) const
    {
        Q_UNUSED(parent);
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
            return QString::fromStdString(node->item.name);
        }

        if (role == Qt::ToolTipRole)
        {
            return QStringLiteral("%1 (%2 unread)")
                .arg(QString::fromStdString(node->item.name))
                .arg(node->item.unreadEmails);
        }

        if (role == MailboxIdRole)
        {
            return QString::fromStdString(node->item.id);
        }

        return {};
    }

    void MailboxTreeModel::setAccountId(std::optional<std::string> accountId)
    {
        if (m_accountId == accountId)
        {
            return;
        }

        m_accountId = std::move(accountId);
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

        if (!m_accountId.has_value())
        {
            endResetModel();
            return;
        }

        const auto result = m_queryService.listMailboxTree(*m_accountId);
        if (const auto* items =
                std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result))
        {
            std::unordered_map<std::string, Node*> nodesById;
            std::vector<javelin::jmap::cache::MailboxTreeItem> roots;
            std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
                deferredChildren;

            auto attachChildren = [&nodesById, &deferredChildren](const auto& self,
                                                                  Node* parentNode) -> void
            {
                const auto deferredIt = deferredChildren.find(parentNode->item.id);
                if (deferredIt == deferredChildren.end())
                {
                    return;
                }

                auto children = std::move(deferredIt->second);
                deferredChildren.erase(deferredIt);
                for (auto& childItem : children)
                {
                    auto child = std::make_unique<Node>();
                    child->item = std::move(childItem);
                    child->parent = parentNode;
                    nodesById.emplace(child->item.id, child.get());
                    parentNode->children.push_back(std::move(child));
                    self(self, parentNode->children.back().get());
                }
            };

            for (const auto& item : *items)
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
                child->item = item;
                child->parent = parentIt->second;
                nodesById.emplace(child->item.id, child.get());
                parentIt->second->children.push_back(std::move(child));
                attachChildren(attachChildren, parentIt->second->children.back().get());
            }

            for (auto& rootItem : roots)
            {
                auto root = std::make_unique<Node>();
                root->item = std::move(rootItem);
                nodesById.emplace(root->item.id, root.get());
                m_rootNodes.push_back(std::move(root));
                attachChildren(attachChildren, m_rootNodes.back().get());
            }
        }

        endResetModel();
    }

} // namespace javelin::gui::mailboxes
