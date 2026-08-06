#pragma once

#include "protocol/ProcessBoundary.h"

#include <optional>

namespace javelin::gui::settings
{
    class GuiSettings;
}

namespace javelin::gui::compose
{
    class ComposeUiPreferences
    {
      public:
        [[nodiscard]] static bool
        richTextDefault(const javelin::gui::settings::GuiSettings& settings);
        [[nodiscard]] static std::optional<javelin::protocol::BoundaryError>
        setRichTextDefault(javelin::gui::settings::GuiSettings& settings, bool richText);
    };
} // namespace javelin::gui::compose
