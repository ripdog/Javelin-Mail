#include "gui/shell/TabActivationPolicy.h"

namespace javelin::gui::shell
{
    TabActivationPlan planTabActivation(const TabActivationContext& context)
    {
        if (!context.kind.has_value())
        {
            return {
                .showMailboxPane = true,
                .clearMessagePresentation = true,
                .refreshRemote = false,
            };
        }

        switch (*context.kind)
        {
        case TabKind::Mailbox:
            return {
                .showMailboxPane = context.homeTab,
                .clearMessagePresentation = false,
                .refreshRemote = true,
            };
        case TabKind::Search:
            return {
                .showMailboxPane = context.homeTab,
                .clearMessagePresentation = false,
                .refreshRemote = context.remoteRefreshRequested || context.messagePageStale,
            };
        case TabKind::Compose:
            return {
                .showMailboxPane = false,
                .clearMessagePresentation = false,
                .refreshRemote = false,
            };
        case TabKind::Contacts:
        case TabKind::Calendar:
            return {
                .showMailboxPane = false,
                .clearMessagePresentation = true,
                .refreshRemote = context.remoteRefreshRequested,
            };
        }

        return {};
    }
} // namespace javelin::gui::shell
