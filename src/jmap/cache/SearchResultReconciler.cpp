#include "jmap/cache/SearchResultReconciler.h"

#include <algorithm>

namespace javelin::jmap::cache
{

    ReconciledSearchResults
    reconcileServerSearchResults(const std::vector<MessageListItem>& current,
                                 const std::vector<MessageListItem>& server,
                                 const std::optional<std::string_view> protectedEmailId)
    {
        ReconciledSearchResults result;
        result.items.reserve(server.size() + (protectedEmailId.has_value() ? 1 : 0));
        const auto protectedItem =
            protectedEmailId.has_value()
                ? std::ranges::find(current, *protectedEmailId, &MessageListItem::emailId)
                : current.end();
        bool protectedThreadIncluded = false;
        for (const auto& serverItem : server)
        {
            if (protectedItem != current.end() && protectedItem->threadId == serverItem.threadId)
            {
                protectedThreadIncluded = true;
                if (protectedItem->emailId == serverItem.emailId)
                {
                    result.items.push_back(serverItem);
                }
                else
                {
                    result.items.push_back(*protectedItem);
                    result.retainedLocalEmailIds.insert(protectedItem->emailId);
                }
            }
            else
            {
                result.items.push_back(serverItem);
            }
        }

        if (protectedItem != current.end() && !protectedThreadIncluded)
        {
            result.items.push_back(*protectedItem);
            result.retainedLocalEmailIds.insert(protectedItem->emailId);
        }
        return result;
    }

} // namespace javelin::jmap::cache
