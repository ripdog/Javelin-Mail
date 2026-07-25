#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace javelin::gui::messages
{
    struct MessageRowIdentity
    {
        std::string threadId;
        std::string emailId;
    };

    struct MessageSelectionRestorationRequest
    {
        std::optional<std::string> threadId;
        std::optional<std::string> emailId;
        std::vector<std::string> selectedEmailIds;
        std::optional<std::size_t> previousRow;
    };

    struct MessageSelectionRestorationPlan
    {
        std::vector<std::size_t> selectedRows;
        std::optional<std::size_t> currentRow;
        bool currentEmailChanged = false;
        bool fallbackSelected = false;
    };

    [[nodiscard]] MessageSelectionRestorationPlan
    planMessageSelectionRestoration(std::span<const MessageRowIdentity> rows,
                                    const MessageSelectionRestorationRequest& request);
} // namespace javelin::gui::messages
