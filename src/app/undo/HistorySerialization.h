#pragma once

#include "app/undo/HistoryTypes.h"

#include <QString>

#include <variant>

namespace javelin::app::undo
{

    struct HistorySerializationError
    {
        QString message;
    };

    struct SerializedHistoryPayload
    {
        int version = 1;
        QString json;
    };

    [[nodiscard]] std::variant<SerializedHistoryPayload, HistorySerializationError>
    serializeHistoryPayload(const HistoryPayload& payload);

    [[nodiscard]] std::variant<HistoryPayload, HistorySerializationError>
    deserializeHistoryPayload(const QString& commandKind, int version, const QString& json);

    [[nodiscard]] QString payloadCommandKind(const HistoryPayload& payload);

} // namespace javelin::app::undo
