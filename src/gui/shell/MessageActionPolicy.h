#pragma once

#include "gui/shell/TabWorkspace.h"

#include <cstddef>
#include <optional>

namespace javelin::gui::shell
{
    struct MessageActionContext
    {
        std::optional<TabKind> tabKind;
        bool hasAccount = false;
        bool hasMailbox = false;
        std::size_t selectedCount = 0;
        bool activeMailboxIsDrafts = false;
        bool hasReadSelection = false;
    };

    struct MessageActionAvailability
    {
        bool newMessage = true;
        bool reply = false;
        bool replyAll = false;
        bool forward = false;
        bool editDraft = false;
        bool archive = false;
        bool markUnread = false;
        bool deleteFromMailbox = false;
        bool permanentDelete = false;
        bool move = false;
        bool copy = false;
        bool viewSource = false;
    };

    [[nodiscard]] MessageActionAvailability
    messageActionAvailability(const MessageActionContext& context);
} // namespace javelin::gui::shell
