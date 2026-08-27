#include "gui/shell/TabActivationPolicy.h"

namespace javelin::gui::shell
{
    TabActivationPlan planTabActivation(const TabActivationContext& context)
    {
        if (!context.kind.has_value())
        {
            return {
                .showMailboxPane = true,
                .refreshRemote = false,
            };
        }

        switch (*context.kind)
        {
        case TabKind::Mailbox:
        case TabKind::Search:
            return {
                .showMailboxPane = context.homeTab,
                .refreshRemote = context.remoteRefreshRequested || context.messageListStale,
            };
        case TabKind::Compose:
            return {
                .showMailboxPane = false,
                .refreshRemote = false,
            };
        case TabKind::Contacts:
        case TabKind::Calendar:
            return {
                .showMailboxPane = false,
                .refreshRemote = context.remoteRefreshRequested,
            };
        }

        return {};
    }
} // namespace javelin::gui::shell
