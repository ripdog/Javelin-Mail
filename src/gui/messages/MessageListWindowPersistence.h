#pragma once

#include "app/MessageListSession.h"

#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace javelin::gui::messages
{
    [[nodiscard]] inline std::vector<javelin::app::MessageListWindowRequest>
    readMessageListWindowManifest(const QVariantMap& settings, const QString& prefix)
    {
        const auto offsets = settings.value(prefix + QStringLiteral("windowOffsets")).toList();
        const auto limits = settings.value(prefix + QStringLiteral("windowLimits")).toList();
        const auto count = std::min<std::size_t>({static_cast<std::size_t>(offsets.size()),
                                                  static_cast<std::size_t>(limits.size()),
                                                  javelin::app::maximumRestoredMessageListWindows});

        std::vector<javelin::app::MessageListWindowRequest> windows;
        windows.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            bool offsetValid = false;
            bool limitValid = false;
            const auto offset = offsets[static_cast<qsizetype>(index)].toULongLong(&offsetValid);
            const auto limit = limits[static_cast<qsizetype>(index)].toULongLong(&limitValid);
            if (!offsetValid || !limitValid || limit == 0)
                break;
            windows.push_back({
                .offset = static_cast<std::size_t>(offset),
                .limit = static_cast<std::size_t>(limit),
            });
        }
        return windows;
    }

    inline void writeMessageListWindowManifest(
        QVariantMap& settings, const QString& prefix,
        const std::vector<javelin::app::MessageListWindowRequest>& windows)
    {
        QVariantList offsets;
        QVariantList limits;
        const auto count =
            std::min(windows.size(), javelin::app::maximumRestoredMessageListWindows);
        offsets.reserve(static_cast<qsizetype>(count));
        limits.reserve(static_cast<qsizetype>(count));
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto& window = windows[index];
            if (window.limit == 0)
                break;
            offsets.push_back(QVariant::fromValue(static_cast<qulonglong>(window.offset)));
            limits.push_back(QVariant::fromValue(static_cast<qulonglong>(window.limit)));
        }

        const auto offsetsKey = prefix + QStringLiteral("windowOffsets");
        const auto limitsKey = prefix + QStringLiteral("windowLimits");
        if (offsets.isEmpty())
        {
            settings.remove(offsetsKey);
            settings.remove(limitsKey);
            return;
        }
        settings.insert(offsetsKey, offsets);
        settings.insert(limitsKey, limits);
    }
} // namespace javelin::gui::messages
