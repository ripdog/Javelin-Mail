#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace javelin::gui::shell
{
    struct MessageContentOwnershipInput
    {
        std::string_view requestAccountId;
        std::string_view requestEmailId;
        std::optional<std::string_view> activeAccountId;
        std::span<const std::string> selectedEmailIds;
        std::optional<std::string_view> routeAccountId;
        std::optional<std::string_view> routeEmailId;
    };

    [[nodiscard]] bool ownsMessageContentResult(const MessageContentOwnershipInput& input);
} // namespace javelin::gui::shell
