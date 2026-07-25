#include "gui/shell/ComposeTabPolicy.h"

#include <algorithm>

namespace javelin::gui::shell
{
    ComposeTabOpenPlan planComposeTabOpen(const std::span<const ComposeTabDescriptor> tabs,
                                          const ComposeTabOpenInput& input)
    {
        const auto existing = std::ranges::find(tabs, input.composeSessionId,
                                                &ComposeTabDescriptor::composeSessionId);
        if (existing != tabs.end())
        {
            return {
                .existingIndex = existing->index,
                .title = input.subject.has_value() ? std::string{*input.subject} : std::string{},
                .updateExistingTitle = input.subject.has_value(),
            };
        }

        return {
            .existingIndex = std::nullopt,
            .title =
                input.subject.has_value() ? std::string{*input.subject} : std::string{"Compose"},
            .updateExistingTitle = false,
        };
    }

    ComposeTabClosePlan planComposeTabClose(const ComposeTabCloseInput& input)
    {
        if (input.operationInFlight)
            return ComposeTabClosePlan::BlockWhileBusy;
        if (input.closeWithoutPrompt)
            return ComposeTabClosePlan::CloseImmediately;
        if (input.emptyDraft)
            return ComposeTabClosePlan::DiscardWorkingCopyAndClose;
        if (input.savedDraft)
            return ComposeTabClosePlan::ConfirmKeepSavedDraft;
        return ComposeTabClosePlan::ConfirmSaveOrDiscard;
    }
} // namespace javelin::gui::shell
