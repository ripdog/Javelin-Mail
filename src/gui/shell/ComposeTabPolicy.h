#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace javelin::gui::shell
{
    struct ComposeTabDescriptor
    {
        std::size_t index = 0;
        std::string_view composeSessionId;
    };

    struct ComposeTabOpenInput
    {
        std::string_view composeSessionId;
        std::optional<std::string_view> subject;
    };

    struct ComposeTabOpenPlan
    {
        std::optional<std::size_t> existingIndex;
        std::string title;
        bool updateExistingTitle = false;
    };

    [[nodiscard]] ComposeTabOpenPlan planComposeTabOpen(std::span<const ComposeTabDescriptor> tabs,
                                                        const ComposeTabOpenInput& input);

    struct ComposeTabCloseInput
    {
        bool operationInFlight = false;
        bool closeWithoutPrompt = false;
        bool emptyDraft = false;
        bool savedDraft = false;
    };

    enum class ComposeTabClosePlan
    {
        BlockWhileBusy,
        CloseImmediately,
        DiscardWorkingCopyAndClose,
        ConfirmKeepSavedDraft,
        ConfirmSaveOrDiscard,
    };

    [[nodiscard]] ComposeTabClosePlan planComposeTabClose(const ComposeTabCloseInput& input);
} // namespace javelin::gui::shell
