#include "jmap/cache/SearchResultReconciler.h"

#include <algorithm>
#include <unordered_map>

namespace javelin::jmap::cache
{

    ReconciledSearchResults
    reconcileServerSearchResults(const std::vector<MessageListItem>& current,
                                 const std::vector<MessageListItem>& server,
                                 const std::optional<std::string_view> protectedEmailId)
    {
        std::unordered_map<std::string_view, const MessageListItem*> serverByThread;
        serverByThread.reserve(server.size());
        for (const auto& item : server)
        {
            serverByThread.emplace(item.threadId, &item);
        }

        ReconciledSearchResults result;
        result.items.reserve(server.size() + (protectedEmailId.has_value() ? 1 : 0));
        std::unordered_set<std::string_view> includedThreads;
        includedThreads.reserve(server.size());
        for (const auto& currentItem : current)
        {
            if (const auto serverItem = serverByThread.find(currentItem.threadId);
                serverItem != serverByThread.end())
            {
                result.items.push_back(*serverItem->second);
                includedThreads.insert(currentItem.threadId);
            }
            else if (protectedEmailId == std::optional<std::string_view>{currentItem.emailId})
            {
                result.items.push_back(currentItem);
                result.retainedLocalEmailIds.insert(currentItem.emailId);
            }
        }

        for (const auto& serverItem : server)
        {
            if (!includedThreads.contains(serverItem.threadId))
            {
                result.items.push_back(serverItem);
            }
        }
        return result;
    }

} // namespace javelin::jmap::cache
