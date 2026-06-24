#include "gui/mailboxes/MailboxIconUtils.h"

#include "gui/IconUtils.h"

namespace javelin::gui::mailboxes
{
    QString mailboxIconResource(const std::optional<std::string>& role)
    {
        if (!role.has_value())
        {
            return QStringLiteral(":/icons/thunderbird-icons/folder.svg");
        }

        if (*role == "inbox")
        {
            return QStringLiteral(":/icons/thunderbird-icons/inbox.svg");
        }
        if (*role == "drafts")
        {
            return QStringLiteral(":/icons/thunderbird-icons/draft.svg");
        }
        if (*role == "sent")
        {
            return QStringLiteral(":/icons/thunderbird-icons/sent.svg");
        }
        if (*role == "junk")
        {
            return QStringLiteral(":/icons/thunderbird-icons/spam.svg");
        }
        if (*role == "trash")
        {
            return QStringLiteral(":/icons/thunderbird-icons/trash.svg");
        }

        return QStringLiteral(":/icons/thunderbird-icons/folder.svg");
    }

    QIcon mailboxIcon(const std::optional<std::string>& role, const QColor& color)
    {
        return javelin::gui::themedSvgIcon(mailboxIconResource(role), color);
    }

} // namespace javelin::gui::mailboxes
