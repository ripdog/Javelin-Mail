#pragma once

#include "gui/shell/MessageTransferDestinationPresentation.h"

#include <functional>

class QMenu;

namespace javelin::gui::shell
{
    using MessageTransferDestinationTriggered =
        std::function<void(const MessageTransferDestinationRow& destination)>;

    [[nodiscard]] bool populateMessageTransferDestinationMenu(
        QMenu& menu, const MessageTransferDestinationPresentation& presentation,
        MessageTransferDestinationTriggered triggered);

} // namespace javelin::gui::shell
