#pragma once

#include "jmap/cache/QueryService.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace javelin::jmap::cache
{

    struct ReconciledSearchResults
    {
        std::vector<MessageListItem> items;
        std::unordered_set<std::string> retainedLocalEmailIds;
    };

    [[nodiscard]] ReconciledSearchResults
    reconcileServerSearchResults(const std::vector<MessageListItem>& current,
                                 const std::vector<MessageListItem>& server,
                                 std::optional<std::string_view> protectedEmailId);

} // namespace javelin::jmap::cache
