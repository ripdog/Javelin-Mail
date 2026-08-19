#include "gui/shell/MessageActionPolicy.h"

namespace javelin::gui::shell
{
    MessageActionAvailability messageActionAvailability(const MessageActionContext& context)
    {
        const auto mailbox = context.tabKind == TabKind::Mailbox;
        const auto search = context.tabKind == TabKind::Search;
        const auto compose = context.tabKind == TabKind::Compose;
        const auto contacts = context.tabKind == TabKind::Contacts;
        const auto calendar = context.tabKind == TabKind::Calendar;
        const auto hasSelection =
            context.hasAccount && context.selectedCount > 0 && !contacts && !calendar;
        const auto movable = (mailbox || search) && context.hasAccount && context.selectedCount > 0;

        return {
            .newMessage = true,
            .reply = hasSelection && !compose,
            .replyAll = hasSelection && !compose,
            .forward = hasSelection && !compose,
            .editDraft = mailbox && context.hasMailbox && context.activeMailboxIsDrafts &&
                         context.selectedCount == 1,
            .archive = movable,
            .markUnread = hasSelection && context.hasReadSelection,
            .star = hasSelection && !compose,
            .junk = hasSelection && !compose,
            .deleteFromMailbox =
                mailbox && context.hasAccount && context.hasMailbox && context.selectedCount > 0,
            .permanentDelete = hasSelection,
            .move = movable,
            .copy = movable,
            .viewSource = hasSelection,
        };
    }
} // namespace javelin::gui::shell
