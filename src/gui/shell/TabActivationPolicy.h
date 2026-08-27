#pragma once

#include "gui/shell/TabWorkspace.h"

#include <optional>

namespace javelin::gui::shell
{
    struct TabActivationContext
    {
        std::optional<TabKind> kind;
        bool homeTab = false;
        bool messageListStale = false;
        bool remoteRefreshRequested = false;
    };

    struct TabActivationPlan
    {
        bool showMailboxPane = true;
        bool refreshRemote = false;
    };

    [[nodiscard]] TabActivationPlan planTabActivation(const TabActivationContext& context);
} // namespace javelin::gui::shell
