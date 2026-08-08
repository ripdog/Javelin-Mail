#pragma once

#include <algorithm>

namespace javelin::gui::messages
{
    [[nodiscard]] inline bool shouldLoadMoreMessages(const int value, const int maximum,
                                                     const int pageStep, const int itemCount)
    {
        if (itemCount <= 0)
            return false;
        const auto threshold = std::max(pageStep * 2, 240);
        return maximum == 0 || maximum - value <= threshold;
    }
} // namespace javelin::gui::messages
