#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace javelin::gui::messages
{
    struct PageMetrics
    {
        std::size_t start = 0;
        std::size_t end = 0;
        bool hasNext = false;
    };

    class PageRefreshState final
    {
      public:
        using Token = std::uint64_t;

        [[nodiscard]] bool begin(const Token token)
        {
            if (m_inFlight)
                return false;
            m_token = token;
            m_inFlight = true;
            return true;
        }

        void supersede()
        {
            m_token = 0;
            m_inFlight = false;
        }

        [[nodiscard]] bool complete(const Token token)
        {
            if (!m_inFlight || token != m_token)
                return false;
            m_token = 0;
            m_inFlight = false;
            return true;
        }

        [[nodiscard]] bool isInFlight() const
        {
            return m_inFlight;
        }

      private:
        Token m_token = 0;
        bool m_inFlight = false;
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
