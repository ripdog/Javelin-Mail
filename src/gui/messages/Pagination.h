#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>

namespace javelin::gui::messages
{
    struct PageMetrics
    {
        std::size_t start = 0;
        std::size_t end = 0;
        bool hasNext = false;
    };

    [[nodiscard]] inline PageMetrics pageMetrics(const std::size_t position,
                                                 const std::size_t representativeCount,
                                                 const std::size_t total)
    {
        if (total == 0 || representativeCount == 0)
            return {};
        return PageMetrics{
            .start = std::min(position + 1, total),
            .end = std::min(position + representativeCount, total),
            .hasNext = position + representativeCount < total,
        };
    }

    [[nodiscard]] inline std::size_t normalizedPageOffset(const std::size_t currentOffset,
                                                          const std::size_t total,
                                                          const std::size_t effectiveLimit)
    {
        if (total == 0)
            return 0;
        if (currentOffset < total)
            return currentOffset;
        const auto step = std::max<std::size_t>(effectiveLimit, 1);
        return ((total - 1) / step) * step;
    }
} // namespace javelin::gui::messages
