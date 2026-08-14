#pragma once

#include "jmap/cache/MailboxReadRepository.h"

#include <QColor>
#include <QIcon>
#include <QString>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace javelin::gui::mailboxes
{
    enum class MailboxPresentationGroup
    {
        SpecialUse,
        User,
    };

    struct MailboxPresentationNode
    {
        std::string accountId;
        javelin::jmap::cache::MailboxTreeItem mailbox;
        MailboxPresentationGroup group = MailboxPresentationGroup::User;
        std::vector<MailboxPresentationNode> children;
    };

    struct MailboxPresentation
    {
        std::string accountId;
        std::vector<MailboxPresentationNode> roots;
    };

    struct MailboxPresentationRow
    {
        const MailboxPresentationNode* node = nullptr;
        std::size_t depth = 0;
        bool separatorBefore = false;
    };

    [[nodiscard]] int specialUseMailboxRank(const std::optional<std::string>& role);
    [[nodiscard]] bool mailboxDisplayLess(const std::optional<std::string>& leftRole,
                                          const std::string& leftName,
                                          const std::optional<std::string>& rightRole,
                                          const std::string& rightName);

    // accountId is deliberately part of every node. A caller may present several forests in one
    // menu later without losing the account half of the destination identity.
    [[nodiscard]] MailboxPresentation
    buildMailboxPresentation(std::string accountId,
                             const std::vector<javelin::jmap::cache::MailboxTreeItem>& mailboxes);
    [[nodiscard]] std::vector<MailboxPresentationRow>
    flattenMailboxPresentation(const MailboxPresentation& presentation);

    [[nodiscard]] QString mailboxIconResource(const std::optional<std::string>& role);
    [[nodiscard]] QIcon mailboxPresentationIcon(const std::optional<std::string>& role,
                                                const QColor& color);
} // namespace javelin::gui::mailboxes
