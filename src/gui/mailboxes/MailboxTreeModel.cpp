#include "gui/mailboxes/MailboxTreeModel.h"

#include "app/MailboxTreeCacheRead.h"
#include "gui/mailboxes/MailboxPresentation.h"
#include "gui/messages/MessageDragPayload.h"

#include <KLocalizedString>

#include <QApplication>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QIcon>
#include <QMimeData>
#include <QPalette>
#include <QSize>
#include <QUrl>
#include <QtConcurrentRun>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace javelin::gui::mailboxes
{
    namespace
    {
        [[nodiscard]] std::vector<javelin::jmap::cache::MailboxTreeItem>
        presentedMailboxes(const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes,
                           const bool includeHidden)
        {
            if (includeHidden)
                return mailboxes;

            std::unordered_map<std::string, const javelin::jmap::cache::MailboxTreeItem*> byId;
            byId.reserve(mailboxes.size());
            for (const auto& mailbox : mailboxes)
                byId.emplace(mailbox.id, &mailbox);

            std::vector<javelin::jmap::cache::MailboxTreeItem> visible;
            visible.reserve(mailboxes.size());
            for (const auto& mailbox : mailboxes)
            {
                if (!mailbox.isSubscribed)
                    continue;
                auto copy = mailbox;
                auto parentId = copy.parentId;
                while (parentId.has_value())
                {
                    const auto parent = byId.find(*parentId);
                    if (parent == byId.end())
                    {
                        parentId.reset();
                        break;
                    }
                    if (parent->second->isSubscribed)
                        break;
                    parentId = parent->second->parentId;
                }
                copy.parentId = std::move(parentId);
                visible.push_back(std::move(copy));
            }
            return visible;
        }

    } // namespace

    MailboxPreferenceState withMailboxPreference(MailboxPreferenceState state,
                                                 const MailboxPreference preference,
                                                 const bool enabled)
    {
        switch (preference)
        {
        case MailboxPreference::Offline:
            state.offline = enabled;
            if (enabled)
                state.hidden = false;
            break;
        case MailboxPreference::Notifications:
            state.notifications = enabled;
            if (enabled)
                state.hidden = false;
            break;
        case MailboxPreference::Hidden:
            state.hidden = enabled;
            if (enabled)
            {
                state.offline = false;
                state.notifications = false;
            }
            break;
        }
        return state;
    }

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

    QModelIndex MailboxTreeModel::index(const int row, const int column,
                                        const QModelIndex& parentIndex) const
    {
        if (column < 0 || column >= columnCount(parentIndex) || row < 0)
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
        return m_options.preferenceColumns ? 4 : 1;
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
            if (index.column() != 0)
                return {};
            if (node->pendingCreate)
                return i18n("%1 (creating…)", QString::fromStdString(node->displayName));
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
                if (node->pendingCreate)
                    return i18n("This mailbox is being created on the server.");
                if (m_options.preferenceColumns)
                {
                    switch (index.column())
                    {
                    case 1:
                        return i18n("Keep a complete offline copy of this mailbox");
                    case 2:
                        return i18n("Show notifications for new mail in this mailbox");
                    case 3:
                        return i18n("Hide this mailbox from the normal mailbox list");
                    default:
                        break;
                    }
                }
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

        if (role == Qt::DecorationRole && index.column() == 0)
        {
            return mailboxPresentationIcon(
                node->role, QApplication::palette().color(QPalette::Active, QPalette::Text));
        }

        if (role == Qt::ForegroundRole && index.column() == 0 &&
            (node->preferences.hidden || node->pendingCreate))
            return QApplication::palette().color(QPalette::Disabled, QPalette::Text);

        if (role == Qt::CheckStateRole && m_options.preferenceColumns && !node->mailboxId.empty())
        {
            switch (index.column())
            {
            case 1:
                return node->preferences.offline ? Qt::Checked : Qt::Unchecked;
            case 2:
                return node->preferences.notifications ? Qt::Checked : Qt::Unchecked;
            case 3:
                return node->preferences.hidden ? Qt::Checked : Qt::Unchecked;
            default:
                return {};
            }
        }

        if (role == Qt::CheckStateRole && m_options.checkable && !node->mailboxId.empty() &&
            index.column() == 0)
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

        if (role == MailboxHiddenRole && !node->mailboxId.empty())
            return m_options.preferenceColumns ? node->preferences.hidden : !node->subscribed;

        if (role == MailboxPendingCreateRole && !node->mailboxId.empty())
            return node->pendingCreate;

        if (role == ConnectionStatusRole && node->kind == Node::Kind::Account)
        {
            const auto status = m_connectionStatuses.find(node->accountId);
            return static_cast<int>(status == m_connectionStatuses.end()
                                        ? ConnectionStatus::Disconnected
                                        : status->second);
        }

        return {};
    }

    QVariant MailboxTreeModel::headerData(const int section, const Qt::Orientation orientation,
                                          const int role) const
    {
        if (!m_options.preferenceColumns || orientation != Qt::Horizontal ||
            role != Qt::DisplayRole)
            return QAbstractItemModel::headerData(section, orientation, role);
        switch (section)
        {
        case 0:
            return i18n("Mailbox");
        case 1:
            return i18n("Offline");
        case 2:
            return i18n("Notifications");
        case 3:
            return i18n("Hide");
        default:
            return {};
        }
    }

    bool MailboxTreeModel::setData(const QModelIndex& index, const QVariant& value, const int role)
    {
        auto* node = index.isValid() ? static_cast<Node*>(index.internalPointer()) : nullptr;
        if (node == nullptr || role != Qt::CheckStateRole || node->mailboxId.empty())
            return false;

        const bool checked = value.toInt() == Qt::Checked;
        if (m_options.preferenceColumns)
        {
            std::optional<MailboxPreference> preference;
            switch (index.column())
            {
            case 1:
                preference = MailboxPreference::Offline;
                break;
            case 2:
                preference = MailboxPreference::Notifications;
                break;
            case 3:
                preference = MailboxPreference::Hidden;
                break;
            default:
                return false;
            }
            const auto updated = withMailboxPreference(node->preferences, *preference, checked);
            if (updated == node->preferences)
                return true;
            node->preferences = updated;
            m_mailboxPreferences.insert_or_assign(node->mailboxId, updated);
            const auto left = createIndex(index.row(), 0, node);
            const auto right = createIndex(index.row(), columnCount(index.parent()) - 1, node);
            Q_EMIT dataChanged(left, right,
                               {Qt::CheckStateRole, Qt::ForegroundRole, MailboxHiddenRole});
            return true;
        }

        if (!m_options.checkable || index.column() != 0)
            return false;
        if (node->checked == checked)
            return true;
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
        if (node->pendingCreate)
        {
            result &= ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled |
                        Qt::ItemIsUserCheckable);
            return result;
        }
        if (!node->mailboxId.empty())
        {
            if (m_options.preferenceColumns)
            {
                if (index.column() > 0)
                    result |= Qt::ItemIsUserCheckable;
            }
            else
            {
                if (node->mayAddItems)
                    result |= Qt::ItemIsDropEnabled;
                if (m_options.checkable && index.column() == 0)
                    result |= Qt::ItemIsUserCheckable;
            }
        }
        return result;
    }

    QStringList MailboxTreeModel::mimeTypes() const
    {
        return {QString::fromLatin1(javelin::gui::messages::messageDragMimeType),
                QStringLiteral("text/uri-list")};
    }

    bool MailboxTreeModel::canDropMimeData(const QMimeData* data, const Qt::DropAction action,
                                           const int row, const int column,
                                           const QModelIndex& parentIndex) const
    {
        Q_UNUSED(row);
        Q_UNUSED(column);
        const auto* node = nodeForIndex(parentIndex);
        if ((action != Qt::MoveAction && action != Qt::CopyAction) || data == nullptr ||
            node == nullptr || node->mailboxId.empty() || !node->mayAddItems)
            return false;

        const auto messageMime = QString::fromLatin1(javelin::gui::messages::messageDragMimeType);
        if (data->hasFormat(messageMime))
        {
            return javelin::gui::messages::decodeMessageDragPayload(data->data(messageMime))
                .has_value();
        }
        if (!data->hasUrls() || data->urls().isEmpty() || action != Qt::CopyAction)
            return false;
        return std::ranges::all_of(data->urls(),
                                   [](const QUrl& url)
                                   {
                                       if (!url.isLocalFile())
                                           return false;
                                       const QFileInfo info{url.toLocalFile()};
                                       return info.exists() && info.isReadable() &&
                                              !info.isSymLink() && (info.isFile() || info.isDir());
                                   });
    }

    bool MailboxTreeModel::dropMimeData(const QMimeData* data, const Qt::DropAction action,
                                        const int row, const int column,
                                        const QModelIndex& parentIndex)
    {
        if (!canDropMimeData(data, action, row, column, parentIndex))
            return false;

        const auto* node = nodeForIndex(parentIndex);
        if (node == nullptr)
            return false;
        const auto messageMime = QString::fromLatin1(javelin::gui::messages::messageDragMimeType);
        if (data->hasFormat(messageMime))
        {
            const auto payload =
                javelin::gui::messages::decodeMessageDragPayload(data->data(messageMime));
            if (!payload.has_value())
                return false;
            Q_EMIT emailsDropped(*payload, QString::fromStdString(node->accountId),
                                 QString::fromStdString(node->mailboxId), action);
            return true;
        }

        QStringList paths;
        paths.reserve(data->urls().size());
        for (const auto& url : data->urls())
            paths.push_back(QFileInfo{url.toLocalFile()}.absoluteFilePath());
        Q_EMIT filesDropped(paths, QString::fromStdString(node->accountId),
                            QString::fromStdString(node->mailboxId));
        return true;
    }

    Qt::DropActions MailboxTreeModel::supportedDropActions() const
    {
        return Qt::CopyAction | Qt::MoveAction;
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
        const auto* cachedMailboxes =
            std::get_if<std::vector<javelin::jmap::cache::MailboxTreeItem>>(&result);
        if (cachedMailboxes == nullptr)
            return false;
        const auto mailboxes = presentedMailboxes(*cachedMailboxes, m_options.includeHidden);

        std::unordered_map<std::string, const javelin::jmap::cache::MailboxTreeItem*> incoming;
        incoming.reserve(mailboxes.size());
        for (const auto& mailbox : mailboxes)
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
                    const auto preference = m_mailboxPreferences.find(node->mailboxId);
                    const auto preferences =
                        preference != m_mailboxPreferences.end()
                            ? preference->second
                            : MailboxPreferenceState{.hidden = !mailbox.isSubscribed};
                    if (node->unreadEmails != mailbox.unreadEmails ||
                        node->totalThreads != mailbox.totalThreads || node->checked != checked ||
                        node->subscribed != mailbox.isSubscribed ||
                        node->mayAddItems != mailbox.myRights.mayAddItems ||
                        node->preferences != preferences)
                    {
                        node->unreadEmails = mailbox.unreadEmails;
                        node->totalThreads = mailbox.totalThreads;
                        node->checked = checked;
                        node->subscribed = mailbox.isSubscribed;
                        node->mayAddItems = mailbox.myRights.mayAddItems;
                        node->preferences = preferences;
                        const auto last = index(row, columnCount(parentIndex) - 1, parentIndex);
                        Q_EMIT dataChanged(current, last,
                                           {Qt::DisplayRole, Qt::ToolTipRole, Qt::CheckStateRole,
                                            Qt::ForegroundRole, TotalThreadsRole,
                                            MailboxHiddenRole});
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

    void MailboxTreeModel::setMailboxPreferences(
        std::unordered_map<std::string, MailboxPreferenceState> preferences)
    {
        for (auto& [mailboxId, state] : preferences)
        {
            Q_UNUSED(mailboxId);
            if (state.hidden)
                state = withMailboxPreference(state, MailboxPreference::Hidden, true);
        }
        if (m_mailboxPreferences == preferences)
            return;
        m_mailboxPreferences = std::move(preferences);
        rebuild();
    }

    std::unordered_map<std::string, MailboxPreferenceState>
    MailboxTreeModel::mailboxPreferences() const
    {
        std::unordered_map<std::string, MailboxPreferenceState> result;
        const auto collect = [&result](const auto& self, const Node* node) -> void
        {
            if (!node->mailboxId.empty())
                result.insert_or_assign(node->mailboxId, node->preferences);
            for (const auto& child : node->children)
                self(self, child.get());
        };
        for (const auto& root : m_rootNodes)
            collect(collect, root.get());
        return result;
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
            if (!account.hasMailCapability)
                continue;
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
            if (!account.hasMailCapability)
                continue;
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
                    : (account.name.empty() ? i18n("Unnamed account").toStdString() : account.name);
            // mailboxId left empty — this is an account-level node.

            const auto mailboxIt = mailboxesByAccount.find(account.accountId);
            const auto mailboxItems =
                mailboxIt == mailboxesByAccount.end()
                    ? std::vector<javelin::jmap::cache::MailboxTreeItem>{}
                    : presentedMailboxes(mailboxIt->second, m_options.includeHidden);

            if (!mailboxItems.empty())
            {
                const auto preferencesFor =
                    [this](const javelin::jmap::cache::MailboxTreeItem& item)
                {
                    const auto found = m_mailboxPreferences.find(item.id);
                    return found != m_mailboxPreferences.end()
                               ? found->second
                               : MailboxPreferenceState{.hidden = !item.isSubscribed};
                };
                const auto makeNode = [this, &preferencesFor](const auto& self,
                                                              const MailboxPresentationNode& item,
                                                              Node* parent) -> std::unique_ptr<Node>
                {
                    auto node = std::make_unique<Node>();
                    node->accountId = item.accountId;
                    node->displayName = item.mailbox.name;
                    node->mailboxId = item.mailbox.id;
                    node->role = item.mailbox.role;
                    node->unreadEmails = item.mailbox.unreadEmails;
                    node->totalThreads = item.mailbox.totalThreads;
                    node->checked = m_options.checkedMailboxIds.contains(
                        QString::fromStdString(item.mailbox.id));
                    node->subscribed = item.mailbox.isSubscribed;
                    node->pendingCreate = item.mailbox.pendingCreate;
                    node->mayAddItems = item.mailbox.myRights.mayAddItems;
                    node->preferences = preferencesFor(item.mailbox);
                    node->parent = parent;
                    for (const auto& child : item.children)
                    {
                        node->children.push_back(self(self, child, node.get()));
                    }
                    return node;
                };

                const auto presentation = buildMailboxPresentation(account.accountId, mailboxItems);
                for (const auto& row : flattenMailboxPresentation(presentation))
                {
                    if (row.depth != 0)
                        continue;
                    if (row.separatorBefore)
                    {
                        auto separator = std::make_unique<Node>();
                        separator->kind = Node::Kind::Separator;
                        separator->accountId = account.accountId;
                        separator->parent = accountNode.get();
                        accountNode->children.push_back(std::move(separator));
                    }
                    accountNode->children.push_back(
                        makeNode(makeNode, *row.node, accountNode.get()));
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
