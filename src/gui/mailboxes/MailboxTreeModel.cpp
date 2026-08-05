#include "gui/mailboxes/MailboxTreeModel.h"

#include "app/MailboxTreeCacheRead.h"
#include "gui/mailboxes/MailboxIconUtils.h"
#include "gui/mailboxes/MailboxSort.h"

#include <KLocalizedString>

#include <QApplication>
#include <QDataStream>
#include <QFutureWatcher>
#include <QIcon>
#include <QMimeData>
#include <QPalette>
#include <QSize>
#include <QtConcurrentRun>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace javelin::gui::mailboxes
{
    namespace
    {
        constexpr auto emailDragMimeType = "application/x-javelin-mail-email-ids";

    } // namespace

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::AccountReader& accountReader,
                                       javelin::jmap::cache::MailboxReader& mailboxReader,
                                       QObject* parent)
        : MailboxTreeModel(accountReader, mailboxReader, Options{}, parent)
    {
    }

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::AccountReader& accountReader,
                                       javelin::jmap::cache::MailboxReader& mailboxReader,
                                       QString databasePath, QObject* parent)
        : MailboxTreeModel(accountReader, mailboxReader, std::move(databasePath), Options{}, parent)
    {
    }

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::AccountReader& accountReader,
                                       javelin::jmap::cache::MailboxReader& mailboxReader,
                                       Options options, QObject* parent)
        : QAbstractItemModel(parent), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_options(std::move(options))
    {
        refresh();
    }

    MailboxTreeModel::MailboxTreeModel(javelin::jmap::cache::AccountReader& accountReader,
                                       javelin::jmap::cache::MailboxReader& mailboxReader,
                                       QString databasePath, Options options, QObject* parent)
        : QAbstractItemModel(parent), m_accountReader(accountReader),
          m_mailboxReader(mailboxReader), m_databasePath(std::move(databasePath)),
          m_options(std::move(options))
    {
        refresh();
    }

    MailboxTreeModel::~MailboxTreeModel() = default;

    bool MailboxTreeModel::mailboxNameLess(const std::unique_ptr<Node>& left,
                                           const std::unique_ptr<Node>& right)
    {
        return mailboxDisplayLess(left->role, left->displayName, right->role, right->displayName);
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
                return i18np("%2 (%1 unread)", "%2 (%1 unread)", node->unreadEmails,
                             QString::fromStdString(node->displayName));
            }
            const auto status = m_connectionStatuses.find(node->accountId);
            const auto connection = status == m_connectionStatuses.end()
                                        ? ConnectionStatus::Disconnected
                                        : status->second;
            QString statusText;
            switch (connection)
            {
            case ConnectionStatus::Disconnected:
                statusText = i18nc("@item account connection state", "Disconnected");
                break;
            case ConnectionStatus::Connecting:
                statusText = i18nc("@item account connection state", "Connecting");
                break;
            case ConnectionStatus::Connected:
                statusText = i18nc("@item account connection state", "Connected");
                break;
            case ConnectionStatus::AuthenticationPaused:
                statusText = i18nc("@item account connection state", "Sign-in required");
                break;
            }
            return QStringLiteral("%1 — %2").arg(QString::fromStdString(node->displayName),
                                                 statusText);
        }

        if (role == Qt::DecorationRole)
        {
            return mailboxIcon(node->role,
                               QApplication::palette().color(QPalette::Active, QPalette::Text));
        }

        if (role == Qt::CheckStateRole && m_options.checkable && !node->mailboxId.empty())
        {
            return node->checked ? Qt::Checked : Qt::Unchecked;
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

        if (role == MailboxNameRole)
        {
            return QString::fromStdString(node->displayName);
        }

        if (role == ConnectionStatusRole && node->kind == Node::Kind::Account)
        {
            const auto status = m_connectionStatuses.find(node->accountId);
            return static_cast<int>(status == m_connectionStatuses.end()
                                        ? ConnectionStatus::Disconnected
                                        : status->second);
        }

        return {};
    }

    bool MailboxTreeModel::setData(const QModelIndex& index, const QVariant& value, const int role)
    {
        auto* node = index.isValid() ? static_cast<Node*>(index.internalPointer()) : nullptr;
        if (node == nullptr || role != Qt::CheckStateRole || !m_options.checkable ||
            node->mailboxId.empty())
        {
            return false;
        }

        const bool checked = value.toInt() == Qt::Checked;
        if (node->checked == checked)
        {
            return true;
        }
        node->checked = checked;
        Q_EMIT dataChanged(index, index, {Qt::CheckStateRole});
        return true;
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
            if (m_options.checkable)
            {
                result |= Qt::ItemIsUserCheckable;
            }
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
        if (!m_databasePath.isEmpty())
        {
            startAsyncRebuild();
            return;
        }
        rebuild();
    }

    bool MailboxTreeModel::refreshAccount(const QStringView accountId)
    {
        if (!m_databasePath.isEmpty())
        {
            refresh();
            return true;
        }
        const auto id = accountId.toString().toStdString();
        const auto result = m_mailboxReader.listMailboxTree(id);
        const auto* mailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
        if (mailboxes == nullptr)
            return false;

        std::unordered_map<std::string, const javelin::jmap::cache::MailboxTreeItem*> incoming;
        incoming.reserve(mailboxes->size());
        for (const auto& mailbox : *mailboxes)
            incoming.emplace(mailbox.id, &mailbox);

        bool structureChanged = false;
        const auto visit = [&](const auto& self, const QModelIndex& parentIndex) -> void
        {
            const int rows = rowCount(parentIndex);
            for (int row = 0; row < rows; ++row)
            {
                const QModelIndex current = index(row, 0, parentIndex);
                auto* node = nodeForIndex(current);
                if (node == nullptr || node->accountId != id)
                {
                    self(self, current);
                    continue;
                }
                if (node->kind == Node::Kind::Mailbox)
                {
                    const auto found = incoming.find(node->mailboxId);
                    if (found == incoming.end())
                    {
                        structureChanged = true;
                        continue;
                    }
                    const auto& mailbox = *found->second;
                    const std::optional<std::string> actualParent =
                        node->parent != nullptr && node->parent->kind == Node::Kind::Mailbox
                            ? std::optional<std::string>{node->parent->mailboxId}
                            : std::nullopt;
                    if (node->displayName != mailbox.name || node->role != mailbox.role ||
                        actualParent != mailbox.parentId)
                    {
                        structureChanged = true;
                        continue;
                    }
                    incoming.erase(found);
                    const bool checked = m_options.checkedMailboxIds.contains(
                        QString::fromStdString(node->mailboxId));
                    if (node->unreadEmails != mailbox.unreadEmails ||
                        node->totalThreads != mailbox.totalThreads || node->checked != checked)
                    {
                        node->unreadEmails = mailbox.unreadEmails;
                        node->totalThreads = mailbox.totalThreads;
                        node->checked = checked;
                        Q_EMIT dataChanged(current, current,
                                           {Qt::DisplayRole, Qt::ToolTipRole, Qt::CheckStateRole,
                                            TotalThreadsRole});
                    }
                }
                self(self, current);
            }
        };
        visit(visit, {});
        structureChanged = structureChanged || !incoming.empty();
        if (structureChanged)
            rebuild();
        return structureChanged;
    }

    void MailboxTreeModel::setAccountId(std::optional<std::string> accountId)
    {
        if (m_options.accountId == accountId)
        {
            return;
        }
        m_options.accountId = std::move(accountId);
        rebuild();
    }

    void MailboxTreeModel::setCheckedMailboxIds(QStringList mailboxIds)
    {
        mailboxIds.removeDuplicates();
        if (m_options.checkedMailboxIds == mailboxIds)
        {
            return;
        }
        m_options.checkedMailboxIds = std::move(mailboxIds);
        rebuild();
    }

    QStringList MailboxTreeModel::checkedMailboxIds() const
    {
        QStringList result;
        const auto collect = [&result](const auto& self, const Node* node) -> void
        {
            if (!node->mailboxId.empty() && node->checked)
            {
                result.push_back(QString::fromStdString(node->mailboxId));
            }
            for (const auto& child : node->children)
            {
                self(self, child.get());
            }
        };
        for (const auto& root : m_rootNodes)
        {
            collect(collect, root.get());
        }
        return result;
    }

    void MailboxTreeModel::setConnectionStatus(const QStringView accountId,
                                               const ConnectionStatus status)
    {
        const auto id = accountId.toString().toStdString();
        if (const auto existing = m_connectionStatuses.find(id);
            existing != m_connectionStatuses.end() && existing->second == status)
        {
            return;
        }
        m_connectionStatuses[id] = status;
        for (int row = 0; row < static_cast<int>(m_rootNodes.size()); ++row)
        {
            if (m_rootNodes[static_cast<std::size_t>(row)]->accountId == id)
            {
                const auto changed = index(row, 0);
                Q_EMIT dataChanged(changed, changed, {ConnectionStatusRole, Qt::ToolTipRole});
                return;
            }
        }
    }

    const MailboxTreeModel::Node* MailboxTreeModel::nodeForIndex(const QModelIndex& index) const
    {
        return index.isValid() ? static_cast<const Node*>(index.internalPointer()) : nullptr;
    }

    MailboxTreeModel::Node* MailboxTreeModel::nodeForIndex(const QModelIndex& index)
    {
        return index.isValid() ? static_cast<Node*>(index.internalPointer()) : nullptr;
    }

    void MailboxTreeModel::rebuild()
    {
        const auto accountsResult = m_accountReader.listAll();
        const auto* accounts =
            std::get_if<std::vector<javelin::jmap::cache::CachedAccount>>(&accountsResult);
        if (accounts == nullptr)
        {
            rebuildFromSnapshot({}, {});
            return;
        }

        std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
            mailboxesByAccount;
        for (const auto& account : *accounts)
        {
            if (m_options.accountId.has_value() && account.accountId != *m_options.accountId)
                continue;
            const auto mailboxResult = m_mailboxReader.listMailboxTree(account.accountId);
            if (const auto* mailboxItems =
                    std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&mailboxResult))
            {
                mailboxesByAccount.emplace(account.accountId, *mailboxItems);
            }
        }
        rebuildFromSnapshot(*accounts, mailboxesByAccount);
    }

    void MailboxTreeModel::startAsyncRebuild()
    {
        if (m_rebuildInFlight)
        {
            m_rebuildPending = true;
            return;
        }

        m_rebuildInFlight = true;
        const auto generation = ++m_rebuildGeneration;
        const auto accountId = m_options.accountId;
        auto* watcher = new QFutureWatcher<javelin::app::MailboxTreeCacheReadResult>(this);
        connect(watcher, &QFutureWatcher<javelin::app::MailboxTreeCacheReadResult>::finished, this,
                [this, watcher, generation]
                {
                    auto result = watcher->result();
                    watcher->deleteLater();
                    m_rebuildInFlight = false;
                    if (generation == m_rebuildGeneration)
                    {
                        if (const auto* snapshot =
                                std::get_if<javelin::app::MailboxTreeCacheSnapshot>(&result))
                        {
                            rebuildFromSnapshot(snapshot->accounts, snapshot->mailboxesByAccount);
                        }
                        else
                        {
                            rebuildFromSnapshot({}, {});
                        }
                    }
                    if (std::exchange(m_rebuildPending, false))
                        startAsyncRebuild();
                });
        watcher->setFuture(
            QtConcurrent::run(javelin::app::loadMailboxTreeCache, m_databasePath, accountId));
    }

    void MailboxTreeModel::rebuildFromSnapshot(
        const std::vector<javelin::jmap::cache::CachedAccount>& accounts,
        const std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>&
            mailboxesByAccount)
    {
        beginResetModel();
        m_rootNodes.clear();

        for (const auto& account : accounts)
        {
            if (m_options.accountId.has_value() && account.accountId != *m_options.accountId)
            {
                continue;
            }
            auto accountNode = std::make_unique<Node>();
            accountNode->kind = Node::Kind::Account;
            accountNode->accountId = account.accountId;
            const auto configuredName =
                m_options.accountDisplayName
                    ? m_options.accountDisplayName(QString::fromStdString(account.accountId))
                    : QString{};
            accountNode->displayName =
                !configuredName.isEmpty()
                    ? configuredName.toStdString()
                    : (account.name.empty() ? account.accountId : account.name);
            // mailboxId left empty — this is an account-level node.

            const auto mailboxIt = mailboxesByAccount.find(account.accountId);
            const auto* mailboxItems =
                mailboxIt == mailboxesByAccount.end() ? nullptr : &mailboxIt->second;

            if (mailboxItems != nullptr)
            {
                std::unordered_map<std::string, Node*> nodesById;
                std::vector<javelin::jmap::cache::MailboxTreeItem> roots;
                std::unordered_map<std::string, std::vector<javelin::jmap::cache::MailboxTreeItem>>
                    deferredChildren;

                auto attachChildren = [&nodesById, &deferredChildren,
                                       &checkedMailboxIds = m_options.checkedMailboxIds,
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
                        child->checked =
                            checkedMailboxIds.contains(QString::fromStdString(childItem.id));
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
                    child->checked =
                        m_options.checkedMailboxIds.contains(QString::fromStdString(item.id));
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
                    rootMailboxNode->checked =
                        m_options.checkedMailboxIds.contains(QString::fromStdString(rootItem.id));
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

                std::ranges::sort(accountNode->children, mailboxNameLess);

                const auto firstOther = std::ranges::find_if(
                    accountNode->children, [](const std::unique_ptr<Node>& child)
                    { return specialUseMailboxRank(child->role) >= 100; });
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

            if (m_options.showAccount)
            {
                m_rootNodes.push_back(std::move(accountNode));
            }
            else
            {
                for (auto& child : accountNode->children)
                {
                    child->parent = nullptr;
                    m_rootNodes.push_back(std::move(child));
                }
            }
        }

        endResetModel();
    }

} // namespace javelin::gui::mailboxes
