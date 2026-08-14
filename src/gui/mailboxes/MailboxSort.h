#pragma once

#include "jmap/cache/MailboxReadRepository.h"

#include <QString>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace javelin::gui::mailboxes
{
    [[nodiscard]] inline int specialUseMailboxRank(const std::optional<std::string>& role)
    {
        if (!role.has_value())
        {
            return 100;
        }

        static constexpr std::array roles{
            std::string_view{"inbox"},     std::string_view{"archive"}, std::string_view{"drafts"},
            std::string_view{"scheduled"}, std::string_view{"sent"},    std::string_view{"junk"},
            std::string_view{"trash"},
        };

        const auto it = std::ranges::find(roles, *role);
        return it == roles.end() ? 100 : static_cast<int>(std::distance(roles.begin(), it));
    }

    [[nodiscard]] inline bool mailboxDisplayLess(const std::optional<std::string>& leftRole,
                                                 const std::string& leftName,
                                                 const std::optional<std::string>& rightRole,
                                                 const std::string& rightName)
    {
        const int leftRank = specialUseMailboxRank(leftRole);
        const int rightRank = specialUseMailboxRank(rightRole);
        if (leftRank != rightRank)
        {
            return leftRank < rightRank;
        }

        return QString::compare(QString::fromStdString(leftName), QString::fromStdString(rightName),
                                Qt::CaseInsensitive) < 0;
    }

    [[nodiscard]] inline std::vector<const javelin::jmap::cache::MailboxTreeItem*>
    mailboxesInDisplayOrder(const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes)
    {
        std::vector<const javelin::jmap::cache::MailboxTreeItem*> ordered;
        ordered.reserve(mailboxes.size());
        for (const auto& mailbox : mailboxes)
        {
            ordered.push_back(&mailbox);
        }

        std::ranges::sort(
            ordered, [](const auto* left, const auto* right)
            { return mailboxDisplayLess(left->role, left->name, right->role, right->name); });
        return ordered;
    }

    [[nodiscard]] inline std::vector<const javelin::jmap::cache::MailboxTreeItem*>
    writableMailboxesInDisplayOrder(
        const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes)
    {
        auto ordered = mailboxesInDisplayOrder(mailboxes);
        std::erase_if(ordered, [](const auto* mailbox) { return !mailbox->myRights.mayAddItems; });
        return ordered;
    }
} // namespace javelin::gui::mailboxes
