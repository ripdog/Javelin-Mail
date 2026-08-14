#include "gui/mailboxes/MailboxPresentation.h"

#include "gui/IconUtils.h"

#include <QString>

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace javelin::gui::mailboxes
{
    int specialUseMailboxRank(const std::optional<std::string>& role)
    {
        if (!role.has_value())
            return 100;

        static constexpr std::array roles{
            std::string_view{"inbox"},     std::string_view{"archive"}, std::string_view{"drafts"},
            std::string_view{"scheduled"}, std::string_view{"sent"},    std::string_view{"junk"},
            std::string_view{"trash"},
        };
        const auto it = std::ranges::find(roles, *role);
        return it == roles.end() ? 100 : static_cast<int>(std::distance(roles.begin(), it));
    }

    bool mailboxDisplayLess(const std::optional<std::string>& leftRole, const std::string& leftName,
                            const std::optional<std::string>& rightRole,
                            const std::string& rightName)
    {
        const int leftRank = specialUseMailboxRank(leftRole);
        const int rightRank = specialUseMailboxRank(rightRole);
        if (leftRank != rightRank)
            return leftRank < rightRank;
        return QString::compare(QString::fromStdString(leftName), QString::fromStdString(rightName),
                                Qt::CaseInsensitive) < 0;
    }

    MailboxPresentation
    buildMailboxPresentation(std::string accountId,
                             const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes)
    {
        MailboxPresentation presentation{.accountId = std::move(accountId), .roots = {}};
        std::unordered_map<std::string, const javelin::jmap::cache::MailboxTreeItem*> byId;
        std::unordered_map<std::string, std::vector<const javelin::jmap::cache::MailboxTreeItem*>>
            childrenByParent;
        byId.reserve(mailboxes.size());
        childrenByParent.reserve(mailboxes.size());
        for (const auto& mailbox : mailboxes)
        {
            byId.emplace(mailbox.id, &mailbox);
            if (mailbox.parentId.has_value())
                childrenByParent[*mailbox.parentId].push_back(&mailbox);
        }

        const auto sortItems = [](auto& items)
        {
            std::ranges::sort(
                items, [](const auto* left, const auto* right)
                { return mailboxDisplayLess(left->role, left->name, right->role, right->name); });
        };
        for (auto& [parentId, children] : childrenByParent)
        {
            Q_UNUSED(parentId);
            sortItems(children);
        }

        std::unordered_set<std::string> emitted;
        std::unordered_set<std::string> visiting;
        const auto appendNode = [&](const auto& self,
                                    const javelin::jmap::cache::MailboxTreeItem& mailbox,
                                    const MailboxPresentationGroup group) -> MailboxPresentationNode
        {
            visiting.insert(mailbox.id);
            MailboxPresentationNode node{
                .accountId = presentation.accountId,
                .mailbox = mailbox,
                .group = group,
                .children = {},
            };
            if (const auto children = childrenByParent.find(mailbox.id);
                children != childrenByParent.end())
            {
                for (const auto* child : children->second)
                {
                    if (visiting.contains(child->id) || emitted.contains(child->id))
                        continue;
                    node.children.push_back(self(self, *child, group));
                }
            }
            visiting.erase(mailbox.id);
            emitted.insert(mailbox.id);
            return node;
        };

        std::vector<const javelin::jmap::cache::MailboxTreeItem*> roots;
        roots.reserve(mailboxes.size());
        for (const auto& mailbox : mailboxes)
        {
            if (!mailbox.parentId.has_value() || !byId.contains(*mailbox.parentId))
                roots.push_back(&mailbox);
        }
        sortItems(roots);
        for (const auto* root : roots)
        {
            if (emitted.contains(root->id))
                continue;
            const auto group = specialUseMailboxRank(root->role) < 100
                                   ? MailboxPresentationGroup::SpecialUse
                                   : MailboxPresentationGroup::User;
            presentation.roots.push_back(appendNode(appendNode, *root, group));
        }

        // Corrupt cyclic parent relationships should remain visible as roots rather than silently
        // disappearing from both the tree and transfer menus.
        std::vector<const javelin::jmap::cache::MailboxTreeItem*> remaining;
        for (const auto& mailbox : mailboxes)
        {
            if (!emitted.contains(mailbox.id))
                remaining.push_back(&mailbox);
        }
        sortItems(remaining);
        for (const auto* root : remaining)
        {
            if (!emitted.contains(root->id))
                presentation.roots.push_back(
                    appendNode(appendNode, *root, MailboxPresentationGroup::User));
        }
        return presentation;
    }

    std::vector<MailboxPresentationRow>
    flattenMailboxPresentation(const MailboxPresentation& presentation)
    {
        std::vector<MailboxPresentationRow> rows;
        const auto append = [&rows](const auto& self, const MailboxPresentationNode& node,
                                    const std::size_t depth, const bool separatorBefore) -> void
        {
            rows.push_back({.node = &node, .depth = depth, .separatorBefore = separatorBefore});
            for (const auto& child : node.children)
                self(self, child, depth + 1, false);
        };

        bool emittedSpecialUseRoot = false;
        bool insertedUserSeparator = false;
        for (const auto& root : presentation.roots)
        {
            const bool separatorBefore = emittedSpecialUseRoot && !insertedUserSeparator &&
                                         root.group == MailboxPresentationGroup::User;
            append(append, root, 0, separatorBefore);
            insertedUserSeparator = insertedUserSeparator || separatorBefore;
            emittedSpecialUseRoot =
                emittedSpecialUseRoot || root.group == MailboxPresentationGroup::SpecialUse;
        }
        return rows;
    }

    QString mailboxIconResource(const std::optional<std::string>& role)
    {
        if (!role.has_value())
            return QStringLiteral(":/icons/thunderbird-icons/folder.svg");
        if (*role == "inbox")
            return QStringLiteral(":/icons/thunderbird-icons/inbox.svg");
        if (*role == "archive")
            return QStringLiteral(":/icons/thunderbird-icons/archive.svg");
        if (*role == "drafts")
            return QStringLiteral(":/icons/thunderbird-icons/draft.svg");
        if (*role == "sent")
            return QStringLiteral(":/icons/thunderbird-icons/sent.svg");
        if (*role == "junk")
            return QStringLiteral(":/icons/thunderbird-icons/spam.svg");
        if (*role == "trash")
            return QStringLiteral(":/icons/thunderbird-icons/trash.svg");
        return QStringLiteral(":/icons/thunderbird-icons/folder.svg");
    }

    QIcon mailboxPresentationIcon(const std::optional<std::string>& role, const QColor& color)
    {
        return javelin::gui::themedSvgIcon(mailboxIconResource(role), color);
    }
} // namespace javelin::gui::mailboxes
