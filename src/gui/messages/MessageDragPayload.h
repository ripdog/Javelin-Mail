#pragma once

#include "app/MessageSelection.h"

#include <QByteArray>
#include <QMetaType>

#include <optional>
#include <string>

namespace javelin::gui::messages
{
    inline constexpr auto messageDragMimeType = "application/x-javelin-mail-transfer-selection";

    struct MessageDragPayload
    {
        std::string sourceAccountId;
        std::optional<std::string> sourceMailboxId;
        javelin::app::MessageSelection selection;
    };

    [[nodiscard]] QByteArray encodeMessageDragPayload(const MessageDragPayload& payload);
    [[nodiscard]] std::optional<MessageDragPayload>
    decodeMessageDragPayload(const QByteArray& payload);

} // namespace javelin::gui::messages

Q_DECLARE_METATYPE(javelin::gui::messages::MessageDragPayload)
