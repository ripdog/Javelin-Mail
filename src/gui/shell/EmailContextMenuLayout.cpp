#include "gui/shell/EmailContextMenuLayout.h"

#include <QSet>

#include <algorithm>

namespace javelin::gui::shell
{
    const QString& emailContextMenuSeparatorId()
    {
        static const QString id = QStringLiteral("separator");
        return id;
    }

    const std::vector<QString>& supportedEmailContextMenuActionIds()
    {
        static const std::vector<QString> ids = {
            QStringLiteral("compose_edit_draft"),
            QStringLiteral("compose_reply"),
            QStringLiteral("compose_reply_all"),
            QStringLiteral("compose_forward"),
            QStringLiteral("archive_email"),
            QStringLiteral("delete_email"),
            QStringLiteral("mark_email_unread"),
            QStringLiteral("toggle_email_starred"),
            QStringLiteral("tag_email"),
            QStringLiteral("move_email"),
            QStringLiteral("copy_email"),
            QStringLiteral("toggle_email_junk"),
            QStringLiteral("find_conversations_with_sender"),
            QStringLiteral("view_message_source"),
            QStringLiteral("permanently_delete_email"),
        };
        return ids;
    }

    const std::vector<QString>& defaultEmailContextMenuLayout()
    {
        static const std::vector<QString> layout = {
            QStringLiteral("compose_edit_draft"),
            QStringLiteral("compose_reply"),
            QStringLiteral("compose_reply_all"),
            QStringLiteral("compose_forward"),
            emailContextMenuSeparatorId(),
            QStringLiteral("archive_email"),
            QStringLiteral("delete_email"),
            QStringLiteral("mark_email_unread"),
            QStringLiteral("toggle_email_starred"),
            emailContextMenuSeparatorId(),
            QStringLiteral("tag_email"),
            QStringLiteral("move_email"),
            QStringLiteral("copy_email"),
            emailContextMenuSeparatorId(),
            QStringLiteral("toggle_email_junk"),
            QStringLiteral("find_conversations_with_sender"),
            emailContextMenuSeparatorId(),
            QStringLiteral("view_message_source"),
            QStringLiteral("permanently_delete_email"),
        };
        return layout;
    }

    std::vector<QString> normalizeEmailContextMenuLayout(const std::vector<QString>& layout)
    {
        const auto& supported = supportedEmailContextMenuActionIds();
        QSet<QString> seenActions;
        std::vector<QString> normalized;
        normalized.reserve(layout.size());
        for (const auto& id : layout)
        {
            if (id == emailContextMenuSeparatorId())
            {
                if (!normalized.empty() && normalized.back() != emailContextMenuSeparatorId())
                    normalized.push_back(id);
                continue;
            }
            if (!std::ranges::contains(supported, id) || seenActions.contains(id))
                continue;
            seenActions.insert(id);
            normalized.push_back(id);
        }
        if (!normalized.empty() && normalized.back() == emailContextMenuSeparatorId())
            normalized.pop_back();
        return normalized;
    }

    std::vector<QString>
    effectiveEmailContextMenuLayout(const std::vector<QString>& configuredLayout)
    {
        return normalizeEmailContextMenuLayout(
            configuredLayout.empty() ? defaultEmailContextMenuLayout() : configuredLayout);
    }

    std::vector<QString> emailContextMenuOverrideForLayout(const std::vector<QString>& layout)
    {
        auto normalized = normalizeEmailContextMenuLayout(layout);
        if (normalized == defaultEmailContextMenuLayout())
            normalized.clear();
        return normalized;
    }
} // namespace javelin::gui::shell
