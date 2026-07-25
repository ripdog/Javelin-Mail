#include "gui/shell/MessageContentPolicy.h"

namespace javelin::gui::shell
{
    bool ownsMessageContentResult(const MessageContentOwnershipInput& input)
    {
        if (!input.activeAccountId.has_value() || *input.activeAccountId != input.requestAccountId)
        {
            return false;
        }

        const bool routeOwnsDetail = input.routeAccountId.has_value() &&
                                     input.routeEmailId.has_value() &&
                                     *input.routeAccountId == input.requestAccountId &&
                                     *input.routeEmailId == input.requestEmailId;
        if (routeOwnsDetail)
            return true;

        return input.selectedEmailIds.size() == 1 &&
               input.selectedEmailIds.front() == input.requestEmailId;
    }
} // namespace javelin::gui::shell
