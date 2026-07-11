#include "gui/mailboxes/MailboxTreeModel.h"

#include "gui/mailboxes/MailboxIconUtils.h"
#include "gui/settings/PreferencesDialog.h"

#include <QApplication>
#include <QDataStream>
#include <QIcon>
#include <QMimeData>
#include <QPalette>
#include <QSize>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace javelin::gui::mailboxes
{
    namespace
    {
        constexpr auto emailDragMimeType = "application/x-javelin-mail-email-ids";

        [[nodiscard]] int primaryMailboxRank(const std::optional<std::string>& role)
        {
            if (!role.has_value())
            {
                return 100;
            }

            static constexpr std::array roles{
                std::string_view{"inbox"},  std::string_view{"archive"},
                std::string_view{"drafts"}, std::string_view{"scheduled"},
                std::string_view{"sent"},   std::string_view{"junk"},
                std::string_view{"trash"},
            };

            const auto it = std::ranges::find(roles, *role);
            return it == roles.end() ? 100 : static_cast<int>(std::distance(roles.begin(), it));
        }

    } // namespace

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::AccountRepository& accountRepository,
                                       javelin::jmap::cache::QueryService& queryService,
                                       QObject* parent)
        : QAbstractItemModel(parent), m_accountRepository(accountRepository),
          m_queryService(queryService)
    {
        rebuild();
    }

    MailboxTreeModel::~MailboxTreeModel() = default;

    bool MailboxTreeModel::mailboxNameLess(const std::unique_ptr<Node>& left,
                                           const std::unique_ptr<Node>& right)
    {
        return QString::compare(QString::fromStdString(left->displayName),
                                QString::fromStdString(right->displayName),
                                Qt::CaseInsensitive) < 0;
    }

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

        if (node->kind == Node::Kind::Separator)
        {
            if (role == Qt::SizeHintRole)
            {
                return QSize{0, 7};
            }
            if (role == Qt::BackgroundRole)
            {
                return QApplication::palette().color(QPalette::Mid);
            }
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

        if (role == Qt::DecorationRole)
        {
            return mailboxIcon(node->role, QApplication::palette().color(QPalette::Text));
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

        if (role == MailboxRoleRole)
        {
            return node->role.has_value() ? QString::fromStdString(*node->role) : QVariant{};
        }

        return {};
    }

    Qt::ItemFlags MailboxTreeModel::flags(const QModelIndex& index) const
    {
        const auto* node = nodeForIndex(index);
        if (node == nullptr || node->kind == Node::Kind::Separator)
        {
            return Qt::NoItemFlags;
        }

        auto result = QAbstractItemModel::flags(index);
        if (!node->mailboxId.empty())
        {
            result |= Qt::ItemIsDropEnabled;
        }
        return result;
    }

    QStringList MailboxTreeModel::mimeTypes() const
    {
        return {QString::fromLatin1(emailDragMimeType)};
    }

    bool MailboxTreeModel::canDropMimeData(const QMimeData* data, const Qt::DropAction action,
                                           const int row, const int column,
                                           const QModelIndex& parentIndex) const
    {
        Q_UNUSED(row);
        Q_UNUSED(column);
        const auto* node = nodeForIndex(parentIndex);
        return action == Qt::MoveAction && data != nullptr &&
               data->hasFormat(QString::fromLatin1(emailDragMimeType)) && node != nullptr &&
               !node->mailboxId.empty();
    }

    bool MailboxTreeModel::dropMimeData(const QMimeData* data, const Qt::DropAction action,
                                        const int row, const int column,
                                        const QModelIndex& parentIndex)
    {
        if (!canDropMimeData(data, action, row, column, parentIndex))
        {
            return false;
        }

        QString sourceAccountId;
        QStringList emailIds;
        auto payload = data->data(QString::fromLatin1(emailDragMimeType));
        QDataStream stream{&payload, QIODeviceBase::ReadOnly};
        stream >> sourceAccountId >> emailIds;
        const auto* node = nodeForIndex(parentIndex);
        if (stream.status() != QDataStream::Ok || sourceAccountId.isEmpty() || emailIds.isEmpty() ||
            node == nullptr)
        {
            return false;
        }

        Q_EMIT emailsDropped(sourceAccountId, QString::fromStdString(node->accountId),
                             QString::fromStdString(node->mailboxId), emailIds);
        return true;
    }

    Qt::DropActions MailboxTreeModel::supportedDropActions() const
    {
        return Qt::MoveAction;
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
            accountNode->kind = Node::Kind::Account;
            accountNode->accountId = account.accountId;
            const auto configuredAccount =
                javelin::gui::settings::PreferencesDialog::loadSettingsForAccount(
                    QString::fromStdString(account.accountId));
            accountNode->displayName =
                !configuredAccount.displayName.isEmpty()
                    ? configuredAccount.displayName.toStdString()
                    : (account.name.empty() ? account.accountId : account.name);
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
                        child->role = childItem.role;
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
                    child->role = item.role;
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
                    rootMailboxNode->role = rootItem.role;
                    rootMailboxNode->unreadEmails = rootItem.unreadEmails;
                    rootMailboxNode->totalThreads = rootItem.totalThreads;
                    rootMailboxNode->parent = accountNode.get();
                    nodesById.emplace(rootMailboxNode->mailboxId, rootMailboxNode.get());
                    accountNode->children.push_back(std::move(rootMailboxNode));
                    attachChildren(attachChildren, accountNode->children.back().get());
                }

                const auto sortChildrenAlphabetically = [](const auto& self, Node* parent) -> void
                {
                    std::ranges::sort(parent->children, mailboxNameLess);
                    for (const auto& child : parent->children)
                    {
                        self(self, child.get());
                    }
                };
                for (const auto& child : accountNode->children)
                {
                    sortChildrenAlphabetically(sortChildrenAlphabetically, child.get());
                }

                std::ranges::sort(
                    accountNode->children,
                    [](const std::unique_ptr<Node>& left, const std::unique_ptr<Node>& right)
                    {
                        const int leftRank = primaryMailboxRank(left->role);
                        const int rightRank = primaryMailboxRank(right->role);
                        if (leftRank != rightRank)
                        {
                            return leftRank < rightRank;
                        }
                        return mailboxNameLess(left, right);
                    });

                const auto firstOther = std::ranges::find_if(
                    accountNode->children, [](const std::unique_ptr<Node>& child)
                    { return primaryMailboxRank(child->role) >= 100; });
                if (firstOther != accountNode->children.begin() &&
                    firstOther != accountNode->children.end())
                {
                    auto separator = std::make_unique<Node>();
                    separator->kind = Node::Kind::Separator;
                    separator->accountId = account.accountId;
                    separator->parent = accountNode.get();
                    accountNode->children.insert(firstOther, std::move(separator));
                }
            }

            m_rootNodes.push_back(std::move(accountNode));
        }

        endResetModel();
    }

} // namespace javelin::gui::mailboxes
