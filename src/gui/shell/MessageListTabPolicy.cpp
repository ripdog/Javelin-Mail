#include "gui/shell/MessageListTabPolicy.h"

#include <algorithm>

namespace javelin::gui::shell
{
    std::optional<std::size_t>
    findReusableMessageListTab(const std::span<const std::optional<MessageListTabIdentity>> tabs,
                               const MessageListTabIdentity& requested,
                               const std::size_t firstIndex)
    {
        for (std::size_t index = std::min(firstIndex, tabs.size()); index < tabs.size(); ++index)
        {
            const auto& candidate = tabs[index];
            if (candidate.has_value() && candidate->collection == requested.collection &&
                candidate->accountId == requested.accountId &&
                candidate->collectionKey == requested.collectionKey)
            {
                return index;
            }
        }
        return std::nullopt;
    }

    std::vector<std::size_t>
    messageListTabsToMarkStale(const std::span<const std::optional<MessageListTabIdentity>> tabs,
                               const std::string_view accountId,
                               const std::optional<std::string_view> refreshedMailboxId,
                               const bool searchesOnly)
    {
        std::vector<std::size_t> indexes;
        for (std::size_t index = 0; index < tabs.size(); ++index)
        {
            const auto& identity = tabs[index];
            if (!identity.has_value() || identity->accountId != accountId)
                continue;

            if (identity->collection == MessageListTabCollection::Search)
            {
                indexes.push_back(index);
                continue;
            }

            if (!searchesOnly &&
                (!refreshedMailboxId.has_value() || identity->collectionKey != *refreshedMailboxId))
            {
                indexes.push_back(index);
            }
        }
        return indexes;
    }
} // namespace javelin::gui::shell
