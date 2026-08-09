#include "gui/messages/MessageSelectionRestoration.h"

#include <algorithm>
#include <unordered_set>

namespace javelin::gui::messages
{
    MessageSelectionRestorationPlan
    planMessageSelectionRestoration(const std::span<const MessageRowIdentity> rows,
                                    const MessageSelectionRestorationRequest& request)
    {
        MessageSelectionRestorationPlan plan;

        if (!request.selectedEmailIds.empty())
        {
            const std::unordered_set<std::string> selectedIds{request.selectedEmailIds.begin(),
                                                              request.selectedEmailIds.end()};
            for (std::size_t row = 0; row < rows.size(); ++row)
            {
                if (!selectedIds.contains(rows[row].emailId))
                    continue;

                plan.selectedRows.push_back(row);
                if (request.emailId == std::optional<std::string>{rows[row].emailId})
                    plan.currentRow = row;
            }

            if (!plan.selectedRows.empty())
            {
                if (!plan.currentRow.has_value())
                    plan.currentRow = plan.selectedRows.back();
                return plan;
            }
        }

        if (request.emailId.has_value())
        {
            const auto email =
                std::ranges::find(rows, *request.emailId, &MessageRowIdentity::emailId);
            if (email != rows.end())
            {
                plan.currentRow = static_cast<std::size_t>(std::distance(rows.begin(), email));
                return plan;
            }
        }

        if (request.threadId.has_value())
        {
            const auto thread =
                std::ranges::find(rows, *request.threadId, &MessageRowIdentity::threadId);
            if (thread != rows.end())
            {
                plan.currentRow = static_cast<std::size_t>(std::distance(rows.begin(), thread));
                plan.currentEmailChanged =
                    request.emailId.has_value() && thread->emailId != *request.emailId;
                return plan;
            }
        }

        if (request.previousRow.has_value() && !rows.empty())
        {
            plan.currentRow = std::min(*request.previousRow, rows.size() - 1);
            plan.fallbackSelected = true;
        }

        return plan;
    }

    bool shouldActivateRestoredSelection(const bool selectionChanged, const bool quickFilterActive)
    {
        return selectionChanged && !quickFilterActive;
    }
} // namespace javelin::gui::messages
