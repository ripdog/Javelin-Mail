#include "gui/compose/ComposeUiPreferences.h"

#include "gui/settings/GuiSettings.h"

#include <utility>

namespace javelin::gui::compose
{
    bool ComposeUiPreferences::richTextDefault(const javelin::gui::settings::GuiSettings& settings)
    {
        return settings.workspaceSettings().composeRichTextDefault;
    }

    std::optional<javelin::protocol::BoundaryError>
    ComposeUiPreferences::setRichTextDefault(javelin::gui::settings::GuiSettings& settings,
                                             const bool richText)
    {
        auto workspace = settings.workspaceSettings();
        if (workspace.composeRichTextDefault == richText)
        {
            return std::nullopt;
        }
        workspace.composeRichTextDefault = richText;
        return settings.updateWorkspace(std::move(workspace));
    }
} // namespace javelin::gui::compose
